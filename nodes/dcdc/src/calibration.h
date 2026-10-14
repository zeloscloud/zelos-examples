/*
 * What a tool may recalibrate while the node runs: XCP by symbol (dcdc.a2l),
 * UDS by address (src/uds.c, window 0x00020000). Built with `-S xcp` or
 * `-S uds`.
 *
 * Both tools store through dcdc_cal_write(), under one lock, and the model
 * reads the pair under it once per cycle: it never runs on one tool's demand
 * and the other's half-written slew rate. An XCP DOWNLOAD carries at most 6
 * bytes, so XCP writes the two floats separately; each lands whole.
 *
 * Precedence: UDS InputOutputControl (0x2F) on 0x0300 overrides the output
 * voltage setpoint (DID 0x0200) while the tester holds it; neither touches
 * demand or slew rate, which only the calibration sets.
 *
 * Security: UDS writes this memory only in extended with level 0x01; XCP
 * writes it without seed and key. This is a development ECU on a bench: any
 * XCP master on the bus may recalibrate it.
 */

#ifndef DCDC_CALIBRATION_H_
#define DCDC_CALIBRATION_H_

#include <stddef.h>

struct dcdc_calibration {
	/* What the 12 V loads ask of the converter, pack side, A; 0..20. */
	float demand_a;
	/* Rate cap on the pack-side current, A/s; 0.1..100. */
	float slew_a_per_s;
};

/* Defined in dcdc.c. */
extern struct dcdc_calibration dcdc_cal;

/* Store len bytes into dcdc_cal under its lock: the XCP and UDS write hook. */
void dcdc_cal_write(void *dst, const void *src, size_t len);

#endif /* DCDC_CALIBRATION_H_ */
