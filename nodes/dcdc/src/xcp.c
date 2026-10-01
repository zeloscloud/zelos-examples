/*
 * What the DC-DC exposes over XCP, built with `-S xcp`.
 *
 * nodes/dcdc/dcdc.a2l describes these variables to a calibration tool by
 * symbol name; the build patches their addresses in from the ELF.
 */

#include "xcp.h"

#include <zelos/xcp.h>

#include <zephyr/sys/util.h>

/* Measurements: this cycle's state, copied out of main()'s locals. */
float dcdc_setpoint_a;
float dcdc_input_current_a;
float dcdc_slew_step_a;
float dcdc_temperature_c;
float dcdc_aux_limit_a;

/* All a master may reach. Only the two calibration parameters are writable. */
static const struct zelos_xcp_region regions[] = {
	{&dcdc_setpoint_a, sizeof(float), false},
	{&dcdc_input_current_a, sizeof(float), false},
	{&dcdc_slew_step_a, sizeof(float), false},
	{&dcdc_temperature_c, sizeof(float), false},
	{&dcdc_aux_limit_a, sizeof(float), false},
	{&dcdc_demand_a, sizeof(float), true},
	{&dcdc_slew_a_per_s, sizeof(float), true},
};

/* One event, once per DCDC_Status cycle. */
static const struct zelos_xcp_event_channel events[] = {
	{"cycle_50ms", 50},
};

static const struct zelos_xcp_config config = {
	.regions = regions,
	.region_count = ARRAY_SIZE(regions),
	.events = events,
	.event_count = ARRAY_SIZE(events),
	.id = "dcdc",
	.epk = zelos_xcp_epk,
};

int dcdc_xcp_start(void)
{
	return zelos_xcp_start(&config);
}

void dcdc_xcp_sample(float setpoint_a, float input_current_a, float slew_step_a,
		     float temperature_c, float aux_limit_a)
{
	dcdc_setpoint_a = setpoint_a;
	dcdc_input_current_a = input_current_a;
	dcdc_slew_step_a = slew_step_a;
	dcdc_temperature_c = temperature_c;
	dcdc_aux_limit_a = aux_limit_a;

	zelos_xcp_event(0);
}
