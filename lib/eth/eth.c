#include <zelos/eth.h>

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/atomic.h>

#if defined(CONFIG_SOC_SERIES_STM32H7X)
#include <soc.h>
#endif

LOG_MODULE_REGISTER(zelos_eth, LOG_LEVEL_INF);

#define IF_EVENTS (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)
#define ACD_EVENTS                                                                                 \
	(NET_EVENT_IPV4_ACD_SUCCEED | NET_EVENT_IPV4_ACD_FAILED | NET_EVENT_IPV4_ACD_CONFLICT)

static struct net_if *iface;
static struct net_in_addr addr;
static struct net_in_addr netmask;
static atomic_t ipv4 = ATOMIC_INIT(ZELOS_ETH_IPV4_NONE);
static struct net_mgmt_event_callback if_cb;
static struct net_mgmt_event_callback acd_cb;

static struct phy_link_state link_state(void)
{
	const struct device *phy = net_eth_get_phy(iface);
	struct phy_link_state link = {0};

	if (phy == NULL || phy_get_link_state(phy, &link) != 0) {
		link.is_up = false;
	}
	return link;
}

/* Adding starts conflict detection, now or at link up. */
static void add_address(void)
{
	if (net_if_ipv4_addr_lookup_by_index(&addr) > 0) {
		return;
	}
	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL ||
	    !net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask)) {
		LOG_ERR("cannot set %s", CONFIG_ZELOS_ETH_IPV4_ADDR);
	}
}

static void link_up(void)
{
	const struct phy_link_state link = link_state();
	const bool fast = PHY_LINK_IS_SPEED_100M(link.speed) != 0;
	const bool full = PHY_LINK_IS_FULL_DUPLEX(link.speed) != 0;

	if (fast && full) {
		LOG_INF("link up, 100 Mb/s full duplex");
	} else {
		LOG_WRN("link up, %s Mb/s %s duplex: expected 100 full", fast ? "100" : "10",
			full ? "full" : "half");
	}
	atomic_set(&ipv4, ZELOS_ETH_IPV4_PROBING);
	add_address();
}

static void on_if(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *ifc)
{
	if (ifc != iface) {
		return;
	}
	if (event == NET_EVENT_IF_UP) {
		link_up();
		return;
	}
	/* A conflict stays visible until the next link up. */
	if (atomic_get(&ipv4) != ZELOS_ETH_IPV4_CONFLICT) {
		atomic_set(&ipv4, ZELOS_ETH_IPV4_NONE);
	}
	LOG_WRN("link down");
}

static void on_acd(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *ifc)
{
	if (ifc != iface) {
		return;
	}
	switch (event) {
	case NET_EVENT_IPV4_ACD_SUCCEED:
		atomic_set(&ipv4, ZELOS_ETH_IPV4_UP);
		LOG_INF("%s up", CONFIG_ZELOS_ETH_IPV4_ADDR);
		break;
	case NET_EVENT_IPV4_ACD_FAILED:
		/* The stack has already dropped the address. */
		atomic_set(&ipv4, ZELOS_ETH_IPV4_CONFLICT);
		LOG_ERR("%s is another host's: not used", CONFIG_ZELOS_ETH_IPV4_ADDR);
		break;
	default:
		/* Claimed by a host after ours was up; the stack keeps and defends it. */
		atomic_set(&ipv4, ZELOS_ETH_IPV4_CONTESTED);
		LOG_ERR("%s also claimed by another host", CONFIG_ZELOS_ETH_IPV4_ADDR);
		break;
	}
}

int zelos_eth_start(void)
{
	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return -ENODEV;
	}
	if (net_addr_pton(NET_AF_INET, CONFIG_ZELOS_ETH_IPV4_ADDR, &addr) != 0 ||
	    net_addr_pton(NET_AF_INET, CONFIG_ZELOS_ETH_IPV4_NETMASK, &netmask) != 0) {
		LOG_ERR("bad ZELOS_ETH_IPV4_ADDR or _NETMASK");
		iface = NULL;
		return -EINVAL;
	}

	net_mgmt_init_event_callback(&if_cb, on_if, IF_EVENTS);
	net_mgmt_add_event_callback(&if_cb);
	net_mgmt_init_event_callback(&acd_cb, on_acd, ACD_EVENTS);
	net_mgmt_add_event_callback(&acd_cb);

	const uint8_t *mac = net_if_get_link_addr(iface)->addr;

	LOG_INF("MAC %02x:%02x:%02x:%02x:%02x:%02x, %s", mac[0], mac[1], mac[2], mac[3], mac[4],
		mac[5], CONFIG_ZELOS_ETH_IPV4_ADDR);
	/* Up before the callbacks were in: Renode's PHY has no autonegotiation. */
	if (net_if_is_up(iface)) {
		link_up();
	} else {
		add_address();
	}
	return 0;
}

/* The MAC's drop counters, read and cleared, added up. */
static uint32_t mac_dropped(void)
{
#if defined(CONFIG_SOC_SERIES_STM32H7X)
	static struct k_spinlock lock;
	static uint32_t total;
	k_spinlock_key_t key = k_spin_lock(&lock);
	/* 11-bit counters, cleared on read; a set overflow bit means at least the maximum. */
	const uint32_t mtl = ETH->MTLRQMPOCR;
	const uint32_t dma = ETH->DMACMFCR;
	uint32_t n;

	total += (mtl & ETH_MTLRQMPOCR_OVFCNTOVF) ? 0x7FF : (mtl & ETH_MTLRQMPOCR_OVFPKTCNT);
	total += (mtl & ETH_MTLRQMPOCR_MISCNTOVF) ? 0x7FF
						    : (mtl & ETH_MTLRQMPOCR_MISPKTCNT) >>
							      ETH_MTLRQMPOCR_MISPKTCNT_Pos;
	total += (dma & ETH_DMACMFCR_MFCO) ? 0x7FF : (dma & ETH_DMACMFCR_MFC);
	n = total;
	k_spin_unlock(&lock, key);
	return n;
#else
	return 0;
#endif
}

enum zelos_eth_ipv4 zelos_eth_ipv4(void)
{
	return (enum zelos_eth_ipv4)atomic_get(&ipv4);
}

struct zelos_eth_status zelos_eth_status(void)
{
	struct zelos_eth_status st = {0};

	if (iface == NULL) {
		return st;
	}

	const struct phy_link_state link = link_state();

	st.link = link.is_up;
	if (link.is_up) {
		st.speed_mbps = PHY_LINK_IS_SPEED_100M(link.speed) ? 100 : 10;
		st.full_duplex = PHY_LINK_IS_FULL_DUPLEX(link.speed) != 0;
	}
	st.ipv4 = (enum zelos_eth_ipv4)atomic_get(&ipv4);
	memcpy(st.mac, net_if_get_link_addr(iface)->addr, sizeof(st.mac));

	struct net_stats_eth eth;

	if (net_mgmt(NET_REQUEST_STATS_GET_ETHERNET, iface, &eth, sizeof(eth)) == 0) {
		st.rx_packets = eth.pkts.rx;
		st.tx_packets = eth.pkts.tx;
		st.rx_errors = eth.errors.rx;
	}
	st.rx_dropped = mac_dropped();
	return st;
}
