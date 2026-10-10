#ifndef DCDC_NODE_H_
#define DCDC_NODE_H_

/**
 * Subscribe, start XCP and UDS if built in, and start the control loop.
 * Needs zelos_can_init() first.
 *
 * @return 0, or negative errno.
 */
int dcdc_start(void);

#endif /* DCDC_NODE_H_ */
