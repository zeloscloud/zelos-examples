/*
 * SOME/IP server on Zephyr sockets: one thread, two UDP sockets (SD on 30490,
 * joined to the SD group, which also programs the MAC's multicast filter; the
 * service's own port), below every CAN protocol's priority.
 */

#include <zelos/eth.h>
#include <zelos/reboot.h>
#include <zelos/someip.h>

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(zelos_someip, LOG_LEVEL_INF);

#define STACK_SIZE 2048
/* With the rest of the network stack (snippets/ethernet), below UDS on CAN (6). */
#define PRIORITY 7
#define POLL_MS 10
/* After a StopOffer, before the reboot: out of the MAC on a LAN. */
#define STOP_OFFER_DRAIN_MS 20

static const struct zelos_someip_service *service;
static struct zelos_someip_core core;
static int sd = -1;
static int svc_fd = -1;
static uint8_t rx[ZELOS_SOMEIP_MESSAGE_MAX];
static atomic_t subscribers;

uint8_t zelos_someip_subscribers(void)
{
	return (uint8_t)atomic_get(&subscribers);
}

static void send_to(int fd, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len)
{
	struct net_sockaddr_in to = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
		.sin_addr.s_addr = net_htonl(addr != 0 ? addr : ZELOS_SOMEIP_SD_GROUP),
	};

	if (fd >= 0 && zsock_sendto(fd, msg, len, 0, (struct net_sockaddr *)&to, sizeof(to)) < 0) {
		LOG_WRN_ONCE("send to %08x:%u: %d", addr, port, errno);
	}
}

static void send_sd(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);
	send_to(sd, addr, port, msg, len);
}

static void send_service(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);
	send_to(svc_fd, addr, port, msg, len);
}

static const struct zelos_someip_ops ops = {
	.send_sd = send_sd,
	.send = send_service,
	.random = sys_rand32_get,
};

static int open_udp(uint16_t port)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
	};
	int s = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);

	if (s < 0 || zsock_bind(s, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("UDP %u: %d", port, errno);
		if (s >= 0) {
			zsock_close(s);
		}
		return -1;
	}
	return s;
}

static bool open_sockets(const struct net_in_addr *self)
{
	struct net_ip_mreqn group = {
		.imr_multiaddr.s_addr = net_htonl(ZELOS_SOMEIP_SD_GROUP),
		.imr_address = *self,
	};

	sd = open_udp(ZELOS_SOMEIP_SD_PORT);
	svc_fd = open_udp(service->port);
	if (sd < 0 || svc_fd < 0) {
		return false;
	}
	/* The group joined (and the MAC's filter set), and multicast sent, on our interface. */
	if (zsock_setsockopt(sd, NET_IPPROTO_IP, ZSOCK_IP_ADD_MEMBERSHIP, &group, sizeof(group)) <
		    0 ||
	    zsock_setsockopt(sd, NET_IPPROTO_IP, ZSOCK_IP_MULTICAST_IF, &group, sizeof(group)) < 0) {
		LOG_ERR("SD group: %d", errno);
		return false;
	}
	return true;
}

static void receive(int fd, bool is_sd)
{
	struct net_sockaddr_in from;
	net_socklen_t from_len = sizeof(from);
	ssize_t n = zsock_recvfrom(fd, rx, sizeof(rx), ZSOCK_MSG_DONTWAIT,
				   (struct net_sockaddr *)&from, &from_len);
	const uint32_t addr = net_ntohl(from.sin_addr.s_addr);
	const uint16_t port = net_ntohs(from.sin_port);

	if (n <= 0) {
		return;
	}
	if (is_sd) {
		zelos_someip_core_sd(&core, addr, port, rx, n, k_uptime_get());
	} else {
		zelos_someip_core_request(&core, addr, port, rx, n);
	}
}

static void run(void *p1, void *p2, void *p3)
{
	bool up = false;
	bool open = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		const bool was_up = up;
		int64_t now = k_uptime_get();
		struct zsock_pollfd fds[2];
		int64_t next;
		int ready;

		up = zelos_eth_ipv4() == ZELOS_ETH_IPV4_UP;
		if (up && !was_up) {
			struct net_in_addr self;

			(void)net_addr_pton(NET_AF_INET, CONFIG_ZELOS_ETH_IPV4_ADDR, &self);
			open = open || open_sockets(&self);
			if (open) {
				zelos_someip_core_up(&core, net_ntohl(self.s_addr), now);
			}
		} else if (!up && was_up) {
			zelos_someip_core_down(&core);
		}
		next = zelos_someip_core_tick(&core, now);
		atomic_set(&subscribers, zelos_someip_core_subscribers(&core));
		if (!open) {
			k_msleep(POLL_MS);
			continue;
		}

		fds[0] = (struct zsock_pollfd){.fd = sd, .events = ZSOCK_POLLIN};
		fds[1] = (struct zsock_pollfd){.fd = svc_fd, .events = ZSOCK_POLLIN};
		ready = zsock_poll(fds, ARRAY_SIZE(fds), (int)CLAMP(next - now, 0, POLL_MS));
		if (ready < 0) {
			/* Never spin on a failing poll. */
			LOG_ERR("poll: %d", errno);
			k_msleep(POLL_MS);
			continue;
		}
		if (fds[0].revents & ZSOCK_POLLIN) {
			receive(sd, true);
		}
		if (fds[1].revents & ZSOCK_POLLIN) {
			receive(svc_fd, false);
		}
	}
}

K_THREAD_DEFINE(someip_tid, STACK_SIZE, run, NULL, NULL, NULL, PRIORITY, 0, SYS_FOREVER_MS);

/*
 * zelos_reboot()'s hook, in the rebooting thread: StopOffer, best effort
 * (PRS_SOMEIPSD_00432). Its own buffer; the session counter may race the
 * SOME/IP thread's, and the reboot follows at once.
 */
static void stop_offer(void)
{
	static uint8_t msg[ZELOS_SOMEIP_MESSAGE_MAX];
	size_t len = zelos_someip_core_stop_offer(&core, msg);

	if (len > 0) {
		send_to(sd, 0, ZELOS_SOMEIP_SD_PORT, msg, len);
		k_msleep(STOP_OFFER_DRAIN_MS);
	}
}

int zelos_someip_offer(const struct zelos_someip_service *svc)
{
	if (svc->event_count > ZELOS_SOMEIP_EVENTS_MAX ||
	    svc->field_count > ZELOS_SOMEIP_FIELDS_MAX) {
		return -EINVAL;
	}
	for (size_t i = 0; i < svc->event_count; i++) {
		if (svc->events[i].size > ZELOS_SOMEIP_PAYLOAD_MAX) {
			return -EINVAL;
		}
	}
	for (size_t i = 0; i < svc->field_count; i++) {
		if (svc->fields[i].size > ZELOS_SOMEIP_PAYLOAD_MAX) {
			return -EINVAL;
		}
	}
	service = svc;
	zelos_someip_core_init(&core, svc, &ops, NULL);
	zelos_reboot_on(stop_offer);
	k_thread_start(someip_tid);
	LOG_INF("SOME/IP service 0x%04x instance %u on UDP %u", svc->service, svc->instance,
		svc->port);
	return 0;
}
