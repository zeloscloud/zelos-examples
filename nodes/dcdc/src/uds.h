/* The DC-DC's UDS view, built with `-S uds`. */

#ifndef DCDC_UDS_H_
#define DCDC_UDS_H_

#include <stdbool.h>

#include "state.h"

int dcdc_uds_start(void);

/* The output voltage to regulate to: the tester's, or the setpoint DID's. */
float dcdc_uds_output_v(void);

/*
 * Publish this cycle's state and run the DTC tests on it. The BMS timeout test
 * runs only while monitored.
 */
void dcdc_uds_sample(const struct dcdc_state *state, bool bms_monitored, bool bms_lost);

#endif /* DCDC_UDS_H_ */
