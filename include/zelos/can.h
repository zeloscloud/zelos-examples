/*
 * Shared CAN plumbing for the nodes.
 *
 * Every node needs the same three operations: bring up the controller, send a
 * frame, and receive one message by ID. The _ext variants carry 29-bit
 * identifiers, for protocols such as J1939.
 */

#ifndef ZELOS_CAN_H_
#define ZELOS_CAN_H_

#include <stdint.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

/**
 * Bring up the board's chosen CAN controller at CONFIG_ZELOS_CAN_BITRATE.
 *
 * @return 0 on success, negative errno otherwise.
 */
int zelos_can_init(void);

/**
 * Send one classic CAN frame with a standard (11-bit) identifier.
 *
 * @param id  Standard identifier.
 * @param data Payload.
 * @param len Payload length, at most 8.
 * @return 0 on success, negative errno otherwise.
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

#endif /* ZELOS_CAN_H_ */
