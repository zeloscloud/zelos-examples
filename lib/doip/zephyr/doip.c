/*
 * DoIP entity on Zephyr sockets: one thread polls UDP 13400, the TCP listener
 * and the tester sockets, below every CAN protocol's priority. UDS runs on the
 * core shared with CAN (zelos_uds_serve()); the response is sent outside its
 * lock, so a slow tester never holds it. A send waits 1 s at most, then the
 * socket closes (its tester's session ends), rather than TCP's retries.
 */

#include <zelos/doip.h>
#include <zelos/eth.h>
#include <zelos/reboot.h>
#include <zelos/uds.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(zelos_doip, LOG_LEVEL_INF);

#define STACK_SIZE 2048
/* With the rest of the network stack (snippets/ethernet), below UDS on CAN (6). */
#define PRIORITY 7
/* Poll at least this often: pending UDS responses, announcements, timers. */
#define POLL_MS 10
#define SEND_TIMEOUT_S 1
/* A_DoIP_Announce_Wait (random, up to), A_DoIP_Announce_Interval, A_DoIP_Announce_Num. */
#define ANNOUNCE_WAIT_MS 500
#define ANNOUNCE_INTERVAL_MS 500
#define ANNOUNCE_NUM 3
/* Vehicle identification responses waiting out A_DoIP_Announce_Wait. */
#define DELAYED_MAX 4
/* Before an ECUReset's reboot: the response, then the FINs, acknowledged on a LAN. */
#define RESET_DRAIN_MS 100
/* Listener, UDP, tester sockets: CONFIG_ZVFS_POLL_MAX at least. */
#define FDS (2 + ZELOS_DOIP_SOCKETS)
BUILD_ASSERT(CONFIG_ZVFS_POLL_MAX >= FDS, "one poll() takes every DoIP socket");

static const struct zelos_doip_config *config;
static struct zelos_doip_core core;
static int fd[ZELOS_DOIP_SOCKETS] = {[0 ... ZELOS_DOIP_SOCKETS - 1] = -1};
static uint8_t reset_after;
static uint8_t rx[ZELOS_DOIP_MESSAGE_MAX];
static uint8_t udp_out[ZELOS_DOIP_MESSAGE_MAX];

static struct {
	struct net_sockaddr_in to;
	int64_t at;
	size_t len;
	uint8_t msg[ZELOS_DOIP_HEADER_SIZE + 32];
} delayed[DELAYED_MAX];

/* A pending routine response from the UDS thread, for one tester. */
static struct k_spinlock pending_lock;
static uint8_t pending[CONFIG_ZELOS_UDS_BUF_SIZE];
static size_t pending_len;
static uint16_t pending_to;

static atomic_t testers;

uint8_t zelos_doip_testers(void)
{
	return (uint8_t)atomic_get(&testers);
}

static bool send_all(void *ctx, int s, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);

	while (len > 0) {
		ssize_t n = zsock_send(fd[s], msg, len, 0);

		if (n <= 0) {
			LOG_WRN("tester socket %d: send failed (%d), closing", s, errno);
			return false;
		}
		msg += n;
		len -= n;
	}
	return true;
}

static void close_socket(void *ctx, int s)
{
	ARG_UNUSED(ctx);

	if (core.socket[s].state == ZELOS_DOIP_ACTIVE) {
		/* Its session, if it held one, ends now rather than at S3. */
		zelos_uds_release(core.socket[s].tester);
		LOG_INF("tester 0x%04x gone", core.socket[s].tester);
	}
	zsock_close(fd[s]);
	fd[s] = -1;
}

static size_t diagnose(void *ctx, uint16_t tester, const uint8_t *req, size_t len,
		       bool functional, uint8_t *rsp)
{
	uint8_t reset;
	size_t n;

	ARG_UNUSED(ctx);

	if (len > CONFIG_ZELOS_UDS_BUF_SIZE) {
		/* Larger than any request the server takes, as on CAN. */
		rsp[0] = 0x7F;
		rsp[1] = req[0];
		rsp[2] = 0x13;
		return 3;
	}
	n = zelos_uds_serve(tester, req, len, functional, rsp, &reset);
	if (reset != ZELOS_UDS_RESET_NONE) {
		reset_after = reset;
	}
	return n;
}

static const struct zelos_doip_ops ops = {
	.send = send_all,
	.close = close_socket,
	.diagnose = diagnose,
};

/* From the UDS thread: copy and return. */
static void on_response(uint16_t origin, const uint8_t *rsp, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&pending_lock);

	memcpy(pending, rsp, MIN(len, sizeof(pending)));
	pending_len = MIN(len, sizeof(pending));
	pending_to = origin;
	k_spin_unlock(&pending_lock, key);
}

static void forward_pending(void)
{
	static uint8_t out[CONFIG_ZELOS_UDS_BUF_SIZE];
	k_spinlock_key_t key = k_spin_lock(&pending_lock);
	size_t len = pending_len;
	uint16_t to = pending_to;

	memcpy(out, pending, len);
	pending_len = 0;
	k_spin_unlock(&pending_lock, key);
	if (len > 0) {
		zelos_doip_core_respond(&core, to, out, len);
	}
}

static int open_udp(void)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(ZELOS_DOIP_PORT),
	};
	/* Zephyr sends broadcasts without SO_BROADCAST, and refuses the option. */
	int s = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);

	if (s < 0 || zsock_bind(s, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("UDP %d: %d", ZELOS_DOIP_PORT, errno);
		return -1;
	}
	return s;
}

static int open_listener(void)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(ZELOS_DOIP_PORT),
	};
	int s = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

	if (s < 0 || zsock_bind(s, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(s, ZELOS_DOIP_SOCKETS) < 0) {
		LOG_ERR("TCP %d: %d", ZELOS_DOIP_PORT, errno);
		return -1;
	}
	return s;
}

static void accept_one(int listener)
{
	const struct timeval timeout = {.tv_sec = SEND_TIMEOUT_S};
	int one = 1;
	int c = zsock_accept(listener, NULL, NULL);
	int s;

	if (c < 0) {
		return;
	}
	s = zelos_doip_core_accept(&core, k_uptime_get());
	if (s < 0) {
		/* Every socket taken, the refusing one too. */
		zsock_close(c);
		return;
	}
	(void)zsock_setsockopt(c, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &timeout, sizeof(timeout));
	(void)zsock_setsockopt(c, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY, &one, sizeof(one));
	fd[s] = c;
}

static void receive(int s)
{
	ssize_t n = zsock_recv(fd[s], rx, sizeof(rx), ZSOCK_MSG_DONTWAIT);

	if (n > 0) {
		zelos_doip_core_receive(&core, s, rx, n, k_uptime_get());
	} else if (n == 0 || errno != EAGAIN) {
		zelos_doip_core_close(&core, s);
	}
}

static void identification(int udp)
{
	struct net_sockaddr_in from;
	net_socklen_t from_len = sizeof(from);
	ssize_t n = zsock_recvfrom(udp, rx, sizeof(rx), ZSOCK_MSG_DONTWAIT,
				   (struct net_sockaddr *)&from, &from_len);
	bool delay;
	size_t len;

	if (n <= 0) {
		return;
	}
	len = zelos_doip_core_udp(&core, rx, n, udp_out, &delay);
	if (len == 0) {
		return;
	}
	if (!delay) {
		(void)zsock_sendto(udp, udp_out, len, 0, (struct net_sockaddr *)&from, sizeof(from));
		return;
	}
	ARRAY_FOR_EACH(delayed, i) {
		if (delayed[i].len == 0) {
			delayed[i].to = from;
			delayed[i].at = k_uptime_get() + sys_rand32_get() % (ANNOUNCE_WAIT_MS + 1);
			delayed[i].len = len;
			memcpy(delayed[i].msg, udp_out, len);
			return;
		}
	}
	/* More requests than slots within 500 ms: this one goes unanswered. */
}

static void send_delayed(int udp, int64_t now)
{
	ARRAY_FOR_EACH(delayed, i) {
		if (delayed[i].len > 0 && now >= delayed[i].at) {
			(void)zsock_sendto(udp, delayed[i].msg, delayed[i].len, 0,
					   (struct net_sockaddr *)&delayed[i].to,
					   sizeof(delayed[i].to));
			delayed[i].len = 0;
		}
	}
}

static void announce(int udp)
{
	struct net_sockaddr_in to = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(ZELOS_DOIP_PORT),
		.sin_addr.s_addr = NET_INADDR_BROADCAST,
	};
	size_t len = zelos_doip_core_announcement(&core, udp_out);

	(void)zsock_sendto(udp, udp_out, len, 0, (struct net_sockaddr *)&to, sizeof(to));
}

static void close_all(void)
{
	for (int s = 0; s < ZELOS_DOIP_SOCKETS; s++) {
		zelos_doip_core_close(&core, s);
	}
	ARRAY_FOR_EACH(delayed, i) {
		delayed[i].len = 0;
	}
}

static void run(void *p1, void *p2, void *p3)
{
	struct zsock_pollfd fds[FDS];
	bool up = false;
	int64_t announce_at = 0;
	int announced = ANNOUNCE_NUM;
	int udp = -1;
	int listener = -1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int64_t now = k_uptime_get();
		int64_t next = zelos_doip_core_tick(&core, now);
		const bool was_up = up;
		int timeout;
		int ready;

		up = zelos_eth_ipv4() == ZELOS_ETH_IPV4_UP;
		if (up && !was_up) {
			if (udp < 0) {
				/* The EID is the MAC, known once lib/eth has started. */
				zelos_doip_core_init(&core, config, zelos_eth_status().mac, &ops,
						     NULL);
				udp = open_udp();
				listener = open_listener();
			}
			announced = 0;
			announce_at = now + sys_rand32_get() % (ANNOUNCE_WAIT_MS + 1);
		} else if (!up && was_up) {
			/* The link went: every tester with it. */
			close_all();
			announced = ANNOUNCE_NUM;
		}
		if (udp >= 0 && announced < ANNOUNCE_NUM && now >= announce_at) {
			announce(udp);
			announced++;
			announce_at = now + ANNOUNCE_INTERVAL_MS;
		}
		if (udp >= 0) {
			send_delayed(udp, now);
		}
		forward_pending();
		atomic_set(&testers, zelos_doip_core_testers(&core));

		fds[0] = (struct zsock_pollfd){.fd = udp, .events = ZSOCK_POLLIN};
		fds[1] = (struct zsock_pollfd){.fd = listener, .events = ZSOCK_POLLIN};
		for (int s = 0; s < ZELOS_DOIP_SOCKETS; s++) {
			fds[2 + s] = (struct zsock_pollfd){.fd = fd[s], .events = ZSOCK_POLLIN};
		}
		timeout = (int)CLAMP(next - now, 0, POLL_MS);
		if (udp < 0) {
			k_msleep(POLL_MS);
			continue;
		}
		ready = zsock_poll(fds, FDS, timeout);
		if (ready < 0) {
			/* Never spin on a failing poll. */
			LOG_ERR("poll: %d", errno);
			k_msleep(POLL_MS);
		}
		if (ready <= 0) {
			continue;
		}
		if (fds[0].revents & ZSOCK_POLLIN) {
			identification(udp);
		}
		if (fds[1].revents & ZSOCK_POLLIN) {
			accept_one(listener);
		}
		for (int s = 0; s < ZELOS_DOIP_SOCKETS; s++) {
			if (fd[s] >= 0 && fds[2 + s].fd == fd[s] && fds[2 + s].revents != 0) {
				receive(s);
			}
		}
		if (reset_after != ZELOS_UDS_RESET_NONE) {
			LOG_INF("ECUReset over DoIP");
			/* The response out first; the hook then closes every tester. */
			k_msleep(RESET_DRAIN_MS);
			zelos_reboot(reset_after == ZELOS_UDS_RESET_HARD ? SYS_REBOOT_COLD
								       : SYS_REBOOT_WARM);
		}
	}
}

K_THREAD_DEFINE(doip_tid, STACK_SIZE, run, NULL, NULL, NULL, PRIORITY, 0, SYS_FOREVER_MS);

/*
 * zelos_reboot()'s hook, in the rebooting thread: every tester learns of the
 * reboot at once (FIN), whatever asked for it. The DoIP thread may still poll
 * these sockets; nothing it does then outlasts the reboot.
 */
static void close_testers(void)
{
	for (int s = 0; s < ZELOS_DOIP_SOCKETS; s++) {
		if (fd[s] >= 0) {
			zsock_close(fd[s]);
		}
	}
	k_msleep(RESET_DRAIN_MS);
}

int zelos_doip_start(const struct zelos_doip_config *cfg)
{
	config = cfg;
	zelos_uds_on_response(on_response);
	zelos_reboot_on(close_testers);
	k_thread_start(doip_tid);
	LOG_INF("DoIP entity 0x%04x (functional 0x%04x)", cfg->logical_address,
		cfg->functional_address);
	return 0;
}
