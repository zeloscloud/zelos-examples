/*
 * GOOSE publisher on an AF_PACKET SOCK_RAW socket: the whole frame, Ethernet
 * header included, handed to the interface. One thread below every CAN
 * protocol's priority, started once the link is up. It sleeps until the next
 * frame is due or zelos_goose_poke(); either way it reads every control block.
 */

#include <zelos/goose.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(zelos_goose, LOG_LEVEL_INF);

#define STACK_SIZE 2048
/* With the rest of the network stack (snippets/ethernet), below UDS on CAN (6). */
#define PRIORITY 7
#define LINK_POLL_MS 100
#define CBS_MAX 4

static struct net_if *iface;
static struct zelos_goose_pub pubs[CBS_MAX];
static size_t pub_count;
static K_SEM_DEFINE(poke, 0, 1);

/* The test knobs: UDS writes them, the thread reads them. */
static atomic_t simulation;
static atomic_t nds_com;
static atomic_t conf_rev = ATOMIC_INIT(1);

void zelos_goose_poke(void)
{
	k_sem_give(&poke);
}

uint8_t zelos_goose_routine(const uint8_t *option, uint8_t *result)
{
	switch (option[0]) {
	case 0x01:
		(void)atomic_xor(&simulation, 1);
		break;
	case 0x02:
		(void)atomic_xor(&nds_com, 1);
		break;
	case 0x03:
		(void)atomic_inc(&conf_rev);
		break;
	default:
		break;
	}
	zelos_goose_poke();
	result[0] = (uint8_t)atomic_get(&simulation);
	result[1] = (uint8_t)atomic_get(&nds_com);
	sys_put_be32((uint32_t)atomic_get(&conf_rev), &result[2]);
	return 6;
}

/*
 * Zephyr's AF_PACKET SOCK_RAW takes protocol 0, ETH_P_ALL, ETH_P_ECAT or
 * ETH_P_IEEE802154 (sockets_packet.c, packet_is_supported), not an EtherType.
 * 0 is send-only: the stack hands it no received frames, where ETH_P_ALL
 * would copy every one on the wire into its queue.
 */
static void run(void *p1, void *p2, void *p3)
{
	static uint8_t frame[ZELOS_GOOSE_FRAME_MAX];
	struct net_sockaddr_ll to = {
		.sll_family = NET_AF_PACKET,
		.sll_protocol = net_htons(ZELOS_GOOSE_ETHERTYPE),
		.sll_halen = 6,
	};
	const struct net_sockaddr *dst = (const struct net_sockaddr *)&to;
	int fd;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!net_if_is_up(iface)) {
		k_msleep(LINK_POLL_MS);
	}
	to.sll_ifindex = net_if_get_by_iface(iface);
	fd = zsock_socket(NET_AF_PACKET, NET_SOCK_RAW, 0);
	if (fd < 0) {
		LOG_ERR("AF_PACKET socket: %d", errno);
		return;
	}
	LOG_INF("%u control blocks on ifindex %d", (unsigned int)pub_count, to.sll_ifindex);

	for (;;) {
		const int64_t now = (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
		const uint8_t *mac = net_if_get_link_addr(iface)->addr;
		int64_t next = INT64_MAX;

		for (size_t i = 0; i < pub_count; i++) {
			struct zelos_goose_pub *pub = &pubs[i];
			const uint32_t state = pub->cb->read(pub->values);
			const uint32_t st_num = pub->st_num;
			size_t len;

			pub->simulation = atomic_get(&simulation) != 0;
			pub->nds_com = atomic_get(&nds_com) != 0;
			zelos_goose_update(pub, state, (uint32_t)atomic_get(&conf_rev), now);
			if (pub->st_num != st_num) {
				LOG_INF("%s stNum %u", pub->cb->go_id, pub->st_num);
			}
			memcpy(to.sll_addr, pub->cb->dst, sizeof(pub->cb->dst));
			while ((len = zelos_goose_poll(pub, now, mac, frame, sizeof(frame))) > 0) {
				ssize_t sent = zsock_sendto(fd, frame, len, ZSOCK_MSG_DONTWAIT, dst,
							    sizeof(to));

				if (sent != (ssize_t)len) {
					LOG_WRN_ONCE("%s: send: %d", pub->cb->go_id, errno);
				}
			}
			next = MIN(next, pub->next_us);
		}
		(void)k_sem_take(&poke, K_TIMEOUT_ABS_US(next));
	}
}

K_THREAD_DEFINE(goose_tid, STACK_SIZE, run, NULL, NULL, NULL, PRIORITY, 0, SYS_FOREVER_MS);

int zelos_goose_start(const struct zelos_goose_cb *cbs, size_t count)
{
	if (count == 0 || count > CBS_MAX) {
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		if (cbs[i].count > ZELOS_GOOSE_MEMBERS_MAX) {
			return -EINVAL;
		}
		pubs[i].cb = &cbs[i];
	}
	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return -ENODEV;
	}
	pub_count = count;
	k_thread_start(goose_tid);
	return 0;
}
