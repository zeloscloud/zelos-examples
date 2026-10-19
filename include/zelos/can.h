/*
 * Shared CAN plumbing for the nodes.
 *
 * lib/can owns the controller: it sets the bitrate and starts it, nothing
 * else reconfigures it, and zelos_can_restart() is the one way to stop it.
 * Every protocol sends through one queue, ordered as the bus arbitrates
 * (lowest identifier first, first in first out within one identifier), which
 * one thread feeds to the controller. The _ext variants carry 29-bit
 * identifiers, for protocols such as J1939. One exception: Zephyr's ISO-TP
 * (lib/uds) sends to the driver directly; see lib/uds/zephyr/uds.c.
 *
 * Receive filters: the STM32 M_CAN hands a frame to the first matching filter
 * only, so two subsystems must never filter one identifier. lib/canopen holds
 * NMT 0x000, SYNC and EMCY 0x080-0x0FF, TIME 0x100 and its node's PDO, SDO and
 * heartbeat identifiers.
 */

#ifndef ZELOS_CAN_H_
#define ZELOS_CAN_H_

#include <stdint.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

/** Transmit counters since boot. */
struct zelos_can_stats {
	/** Frames the bus acknowledged. */
	uint32_t sent;
	/**
	 * Frames lost to a full queue: refused to a sender that gave up, or
	 * dropped by one that held them for a retry.
	 */
	uint32_t overflow;
	/** Refusals of zelos_can_try_queue(), whose sender keeps the frame and retries. */
	uint32_t deferred;
	/**
	 * Frames the controller failed to send, e.g. while bus-off, and frames
	 * dropped as older than CONFIG_ZELOS_CAN_TX_MAX_AGE_MS.
	 */
	uint32_t tx_error;
	/** Most frames the queue has held at once. */
	uint32_t peak;
	/** Frames the controller's RX FIFOs lost; 0 without CONFIG_CAN_STATS. */
	uint32_t rx_overrun;
};

/**
 * Bring up the board's chosen CAN controller at CONFIG_ZELOS_CAN_BITRATE and
 * start the transmit thread. Call once, before any protocol starts.
 *
 * @return 0 on success, negative errno otherwise.
 */
int zelos_can_init(void);

/**
 * Queue one classic frame for transmission. Safe from an ISR with K_NO_WAIT.
 *
 * @param frame The frame, copied.
 * @param timeout How long to wait for room in the queue; K_NO_WAIT to not wait.
 * @return 0 once queued, -EAGAIN if the queue stayed full (counted as
 *         overflow).
 */
int zelos_can_queue(const struct can_frame *frame, k_timeout_t timeout);

/**
 * zelos_can_queue() for a sender that keeps the frame and retries, such as the
 * CANopen driver: a refusal counts as deferred, not overflow.
 *
 * @return 0 once queued, -EAGAIN if the queue is full.
 */
int zelos_can_try_queue(const struct can_frame *frame);

/** Count a frame a zelos_can_try_queue() sender gave up on as overflow. */
void zelos_can_dropped(void);

/**
 * Queue one classic CAN frame with a standard (11-bit) identifier, waiting up
 * to 10 ms for room, so a node that cannot transmit keeps its own schedule.
 *
 * @param id  Standard identifier.
 * @param data Payload.
 * @param len Payload length, at most 8.
 * @return 0 once queued, negative errno otherwise.
 */
int zelos_can_send(uint32_t id, const uint8_t *data, uint8_t len);

/**
 * Route frames carrying exactly this standard identifier into a queue.
 *
 * The queue holds struct can_frame items. Use k_msgq_get() to read them.
 *
 * @param id Standard identifier to match.
 * @param msgq Destination queue.
 * @return Filter id (>= 0) on success, negative errno otherwise.
 */
int zelos_can_subscribe(uint32_t id, struct k_msgq *msgq);

/**
 * Told when one frame left: acknowledged (error 0), failed, dropped as stale,
 * or never queued. Called from the controller's ISR or the sender's thread.
 * Embed it in the sender's state: it must outlive the frame.
 */
struct zelos_can_done {
	void (*fn)(struct zelos_can_done *done, int error);
};

/**
 * zelos_can_send() that reports the frame's fate to done, for a sender that
 * keeps at most one frame in flight so it cannot fill the controller's
 * buffers. done is also called, with the error, when queueing fails.
 */
int zelos_can_send_done(uint32_t id, const uint8_t *data, uint8_t len, struct zelos_can_done *done);

/** zelos_can_send() with an extended (29-bit) identifier. */
int zelos_can_send_ext(uint32_t id, const uint8_t *data, uint8_t len);

/**
 * Route frames whose extended identifier matches id under mask into a queue.
 *
 * @param id Extended identifier to match.
 * @param mask Bits of id that must match, e.g. only a J1939 PGN's PF byte.
 * @param msgq Destination queue of struct can_frame.
 * @return Filter id (>= 0) on success, negative errno otherwise.
 */
int zelos_can_subscribe_ext(uint32_t id, uint32_t mask, struct k_msgq *msgq);

/**
 * Stop the controller, wait `off`, start it again. Stopping drops the frames
 * the controller holds, failing each to its sender with -ENETDOWN: with nothing
 * on the bus to acknowledge a frame, the controller retransmits it for ever.
 * The bitrate and filters stay. Waits first for the queue's thread to leave
 * the driver, up to CONFIG_ZELOS_CAN_TX_MAX_AGE_MS. A sender outside lib/can
 * must not be waiting in can_send() meanwhile (see can.c).
 */
int zelos_can_restart(k_timeout_t off);

/** Transmit counters, e.g. to expose over a diagnostic service. */
struct zelos_can_stats zelos_can_stats(void);

#endif /* ZELOS_CAN_H_ */
