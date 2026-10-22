/*
 * The board's Ethernet: link and one static IPv4 address.
 *
 * zelos_eth_start() sets CONFIG_ZELOS_ETH_IPV4_ADDR on the Ethernet interface
 * and returns at once: nothing waits for a cable, so CAN starts on time
 * without one. The stack brings the address up when the link comes up, after
 * RFC 5227 conflict detection. On a conflict the address is dropped (another
 * host has it) and tried again at the next link up.
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
	/* Another host holds the address; unused until the next link up. */
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
	/* Frames the driver dropped or failed to receive. */
	uint32_t rx_errors;
};

/** Set the address and return. 0, or -ENODEV without an Ethernet interface. */
int zelos_eth_start(void);

/** A snapshot, from any thread. Zeroes before zelos_eth_start(). */
struct zelos_eth_status zelos_eth_status(void);

#endif /* ZELOS_ETH_H_ */
