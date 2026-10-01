/*
 * SAE J1939 for a node: address claim, requests, periodic messages and
 * broadcast transport (TP.BAM) for messages longer than one frame.
 *
 * The core is sans-IO: it takes received frames and the time, and hands
 * frames to send to a callback. It never blocks or sleeps, so it runs the same
 * under ztest, in Renode and on hardware. The Zephyr glue at the end feeds it
 * from the CAN controller.
 */

#ifndef ZELOS_J1939_H_
#define ZELOS_J1939_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZELOS_J1939_PGN_ACK             0xE800U /* 59392 */
#define ZELOS_J1939_PGN_REQUEST         0xEA00U /* 59904 */
#define ZELOS_J1939_PGN_TP_DT           0xEB00U /* 60160 */
#define ZELOS_J1939_PGN_TP_CM           0xEC00U /* 60416 */
#define ZELOS_J1939_PGN_ADDRESS_CLAIMED 0xEE00U /* 60928 */

#define ZELOS_J1939_ADDR_NULL   0xFEU /* source of Cannot Claim Address */
#define ZELOS_J1939_ADDR_GLOBAL 0xFFU

/* J1939-81: after a claim, wait this long for a contender before using it. */
#define ZELOS_J1939_CLAIM_WAIT_MS 250U

/*
 * J1939-21 spaces BAM packets 50 to 200 ms apart. Above the low end, so a
 * packet that waits in the controller's queue still leaves 50 ms on the wire.
 */
#define ZELOS_J1939_BAM_GAP_MS 55U

/* Longest message this core sends. DM1 with 15 DTCs fits. */
#define ZELOS_J1939_MAX_LEN 64U

/* The 64-bit NAME, J1939-81 field order. Lower NAME wins address contention. */
#define ZELOS_J1939_NAME(arbitrary, industry, vsys_instance, vsys, function, function_instance, \
			 ecu_instance, manufacturer, identity)                                  \
	(((uint64_t)(arbitrary) << 63) | ((uint64_t)(industry) << 60) |                        \
	 ((uint64_t)(vsys_instance) << 56) | ((uint64_t)(vsys) << 49) |                        \
	 ((uint64_t)(function) << 40) | ((uint64_t)(function_instance) << 35) |                \
	 ((uint64_t)(ecu_instance) << 32) | ((uint64_t)(manufacturer) << 21) |                 \
	 (uint64_t)(identity))

/* One frame with a 29-bit identifier. */
struct zelos_j1939_frame {
	uint32_t id;
	uint8_t len;
	uint8_t data[8];
};

/*
 * A message the node sends: periodically, when requested, or both. Always to
 * global, so PDU2 PGNs only. Longer than 8 bytes goes by BAM, never RTS/CTS,
 * even when the request was addressed to this node.
 */
struct zelos_j1939_msg {
	uint32_t pgn;
	uint8_t priority;
	/* 0: only on request. */
	uint16_t period_ms;
	/* At most ZELOS_J1939_MAX_LEN. */
	uint16_t len;
	uint8_t *data;

	/* Owned by the core. */
	uint32_t due_ms;
	bool requested;
};

enum zelos_j1939_state {
	ZELOS_J1939_CLAIMING,
	ZELOS_J1939_CLAIMED,
	ZELOS_J1939_CANNOT_CLAIM,
};

struct zelos_j1939 {
	/* Set by the caller before zelos_j1939_init(). */
	uint64_t name;
	uint8_t preferred;
	struct zelos_j1939_msg *msgs;
	size_t n_msgs;
	void (*send)(const struct zelos_j1939_frame *frame, void *user);
	void *user;

	/* Read-only for the caller. */
	enum zelos_j1939_state state;
	uint8_t address;

	/* Owned by the core. */
	uint32_t claimed_ms;
	uint32_t claim_due_ms;
	bool claim_pending;
	bool claim_started;
	uint8_t tried;
	bool nack_pending;
	uint8_t nack_to;
	uint32_t nack_pgn;
	struct {
		bool active;
		uint32_t pgn;
		uint16_t len;
		uint8_t seq;
		uint8_t packets;
		uint32_t due_ms;
		uint8_t buf[ZELOS_J1939_MAX_LEN];
	} bam;
};

/*
 * Start claiming the preferred address. Sends nothing until the next poll.
 * Returns -EINVAL if a message has a PDU1 PGN.
 */
int zelos_j1939_init(struct zelos_j1939 *j, uint32_t now_ms);

/* Feed one received frame. Sends nothing; replies go out on the next poll. */
void zelos_j1939_on_frame(struct zelos_j1939 *j, const struct zelos_j1939_frame *frame,
			  uint32_t now_ms);

/* Send whatever is due: claims, replies, periodic messages, the next BAM packet. */
void zelos_j1939_poll(struct zelos_j1939 *j, uint32_t now_ms);

/*
 * Zephyr glue. Needs zelos_can_init() first. One J1939 node per firmware: this
 * subscribes to requests and claims, sets j->send, and runs the core on its own
 * thread every CONFIG_ZELOS_J1939_TICK_MS. At most 8 messages.
 */
int zelos_j1939_start(struct zelos_j1939 *j);

/* Replace a message's payload. msg->data must hold len bytes. */
void zelos_j1939_set(struct zelos_j1939_msg *msg, const uint8_t *data, uint16_t len);

#endif /* ZELOS_J1939_H_ */
