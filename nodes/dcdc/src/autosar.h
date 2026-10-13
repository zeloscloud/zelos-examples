/* The DC-DC's AUTOSAR PDUs, built with `-S autosar`. */

#ifndef DCDC_AUTOSAR_H_
#define DCDC_AUTOSAR_H_

#include <stdbool.h>

#include "state.h"

int dcdc_autosar_start(void);

/*
 * Publish this cycle's state. derating: above the derate temperature. tx: the
 * node sends its normal messages (UDS CommunicationControl), these included.
 */
void dcdc_autosar_sample(const struct dcdc_state *state, bool derating, bool tx);

#endif /* DCDC_AUTOSAR_H_ */
