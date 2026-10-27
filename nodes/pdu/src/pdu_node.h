#ifndef PDU_NODE_H_
#define PDU_NODE_H_

#include <stdint.h>

/**
 * Start the CANopen stack and the PDU's logic. Needs zelos_can_init() first.
 *
 * @return 0, or negative errno.
 */
int pdu_start(void);

/*
 * Channels powered and trip-latched (bit n: channel n + 1), and overcurrent
 * trips reported by EMCY since boot. From any thread.
 */
void pdu_status(uint8_t *powered, uint8_t *tripped, int32_t *emcy);

/* Toggle channel 1..8's commanded state (0x6200), as a master's write would. */
void pdu_toggle_output(int channel);

#endif /* PDU_NODE_H_ */
