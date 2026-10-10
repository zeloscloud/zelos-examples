#ifndef PDU_NODE_H_
#define PDU_NODE_H_

/**
 * Start the CANopen stack and the PDU's logic. Needs zelos_can_init() first.
 *
 * @return 0, or negative errno.
 */
int pdu_start(void);

#endif /* PDU_NODE_H_ */
