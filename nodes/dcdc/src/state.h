/*
 * One cycle of the DC-DC model, as UDS and AUTOSAR report it. Also UDS's
 * read-only memory window at 0x00010000, as is: 32 bytes, little-endian.
 */

#ifndef DCDC_STATE_H_
#define DCDC_STATE_H_

#include <stdint.h>

struct dcdc_state {
	float output_v;
	float output_a;
	/* Pack voltage from BMS_Status; NaN when none is fresh. */
	float input_v;
	float input_a;
	float temperature_c;
	float setpoint_v;
	float aux_limit_a;
	uint32_t uptime_ms;
};

#endif /* DCDC_STATE_H_ */
