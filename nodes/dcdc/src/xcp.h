/* The DC-DC's XCP view, built with `-S xcp`. */

#ifndef DCDC_XCP_H_
#define DCDC_XCP_H_

/* Calibration parameters, defined in main.c. */
extern float dcdc_demand_a;
extern float dcdc_slew_a_per_s;

int dcdc_xcp_start(void);

/* Publish this cycle's state for measurement and sample the DAQ event. */
void dcdc_xcp_sample(float setpoint_a, float input_current_a, float slew_step_a,
		     float temperature_c, float aux_limit_a);

#endif /* DCDC_XCP_H_ */
