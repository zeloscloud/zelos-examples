/*
 * Shared CAN plumbing for the bench nodes.
 *
 * Every node needs the same three operations: bring up the controller, send a
 * frame, and receive one message by ID.
 */

#ifndef BENCH_BENCH_CAN_H_
#define BENCH_BENCH_CAN_H_

#include <stdint.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

/* Every node runs at this bitrate. See the comment in bench_can_init(). */
#define BENCH_CAN_BITRATE 500000

/**
 * Bring up the board's chosen CAN controller.
 *
 * @return 0 on success, negative errno otherwise.
 */
int bench_can_init(void);

/**
 * Send one classic CAN frame with a standard (11-bit) identifier.
 *
 * @param id  Standard identifier.
 * @param data Payload.
 * @param len Payload length, at most 8.
 * @return 0 on success, negative errno otherwise.
 */
int bench_can_send(uint32_t id, const uint8_t *data, uint8_t len);

/**
 * Route frames carrying exactly this standard identifier into a queue.
 *
 * The queue holds struct can_frame items. Use k_msgq_get() to read them.
 *
 * @param id Standard identifier to match.
 * @param msgq Destination queue.
 * @return Filter id (>= 0) on success, negative errno otherwise.
 */
int bench_can_subscribe(uint32_t id, struct k_msgq *msgq);

#endif /* BENCH_BENCH_CAN_H_ */
