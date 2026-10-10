#ifndef GENSET_NODE_H_
#define GENSET_NODE_H_

/**
 * Subscribe to VCU_Command, start J1939 and the engine model.
 * Needs zelos_can_init() first.
 *
 * @return 0, or negative errno.
 */
int genset_start(void);

#endif /* GENSET_NODE_H_ */
