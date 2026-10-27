/*
 * GOOSE publisher on an AF_PACKET SOCK_RAW socket: the whole frame, Ethernet
 * header included, handed to the interface. One thread below every CAN
 * protocol's priority, started once the link is up. It sleeps until the next
 * frame is due or zelos_goose_poke(); either way it reads every on-change
 * control block, and a periodic one whose frame is due.
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
/* The thread's, and zelos_goose_start()'s before the thread runs. */
static uint8_t frame[ZELOS_GOOSE_FRAME_MAX];

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
			const uint32_t st_num = pub->st_num;
			uint32_t state;
			size_t len;

			/* Periodic: read when due, so each frame's stNum is the last's + 1. */
			if (pub->cb->period_ms != 0 && st_num != 0 && now < pub->next_us) {
				next = MIN(next, pub->next_us);
				continue;
			}
			state = pub->cb->read(pub->values);
			pub->simulation = atomic_get(&simulation) != 0;
			pub->nds_com = atomic_get(&nds_com) != 0;
			zelos_goose_update(pub, state, (uint32_t)atomic_get(&conf_rev), now);
			/* A periodic block's stNum may change every frame. */
			if (pub->st_num != st_num && pub->cb->period_ms == 0) {
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

/* Whether pub's longest frame fits: every counter and integer at its longest encoding. */
static bool fits(struct zelos_goose_pub *pub)
{
	static const uint8_t mac[6];

	(void)pub->cb->read(pub->values);
	for (size_t k = 0; k < pub->cb->count; k++) {
		struct zelos_goose_value *v = &pub->values[k];

		if (v->type == ZELOS_GOOSE_INT32 || v->type == ZELOS_GOOSE_UINT32) {
			/* INT32_MIN, or 2^31: 4 and 5 octets. */
			v->u32 = 0x80000000U;
		}
	}
	zelos_goose_update(pub, 0, UINT32_MAX, 0);
	pub->st_num = UINT32_MAX;
	pub->sq_num = UINT32_MAX;
	/* timeAllowedtoLive at its steady 2000 ms, on change. */
	pub->step = ZELOS_GOOSE_BURST;
	return zelos_goose_poll(pub, 0, mac, frame, sizeof(frame)) > 0;
}

int zelos_goose_start(const struct zelos_goose_cb *cbs, size_t count)
{
	if (count == 0 || count > CBS_MAX) {
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		if (cbs[i].count > ZELOS_GOOSE_MEMBERS_MAX) {
			return -EINVAL;
		}
		pubs[i] = (struct zelos_goose_pub){.cb = &cbs[i]};
		if (!fits(&pubs[i])) {
			LOG_ERR("%s: frame over %d octets", cbs[i].go_id, ZELOS_GOOSE_FRAME_MAX);
			return -EINVAL;
		}
		pubs[i] = (struct zelos_goose_pub){.cb = &cbs[i]};
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
