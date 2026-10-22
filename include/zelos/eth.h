/*
 * The board's Ethernet: link and one static IPv4 address.
 *
 * zelos_eth_start() sets CONFIG_ZELOS_ETH_IPV4_ADDR on the Ethernet interface
 * and returns at once: nothing waits for a cable, so CAN starts on time
 * without one. The stack brings the address up when the link comes up, after
 * RFC 5227 conflict detection, about 5 s after link up. A conflict while
 * probing leaves the address unused until the next link up; one after it is
 * up is reported and the address kept, as Zephyr does.
 */

#ifndef ZELOS_ETH_H_
#define ZELOS_ETH_H_

#include <stdbool.h>
#include <stdint.h>

enum zelos_eth_ipv4 {
	/* No link, or no address. */
	ZELOS_ETH_IPV4_NONE = 0,
	/* Conflict detection running. */
	ZELOS_ETH_IPV4_PROBING = 1,
	ZELOS_ETH_IPV4_UP = 2,
	/* Another host claims the address. */
	ZELOS_ETH_IPV4_CONFLICT = 3,
};

struct zelos_eth_status {
	bool link;
	/* 10 or 100 while the link is up, else 0. */
	uint8_t speed_mbps;
	bool full_duplex;
	enum zelos_eth_ipv4 ipv4;
	uint8_t mac[6];
	/* Since boot. */
	uint32_t rx_packets;
	uint32_t tx_packets;
	/*
	 * Frames dropped on receive, Zephyr's count: not for this host (another
	 * host's ARP, flooded unicast), refused by the stack, or lost (no buffer,
	 * DMA error). On a shared LAN it grows; not a loss count alone.
	 */
	uint32_t rx_errors;
	/*
	 * Frames the MAC itself dropped, receive FIFO full or no DMA descriptor
	 * (STM32H7: MTL RX missed/overflow and DMA missed frame counters): the
	 * loss count. 0 where the MAC has none.
	 */
	uint32_t rx_dropped;
};

/** Set the address and return. 0, or -ENODEV without an Ethernet interface. */
int zelos_eth_start(void);

/** The address's state alone, cheaply, from any thread. */
enum zelos_eth_ipv4 zelos_eth_ipv4(void);

/** A snapshot, from any thread. Zeroes before zelos_eth_start(). */
struct zelos_eth_status zelos_eth_status(void);

#endif /* ZELOS_ETH_H_ */
