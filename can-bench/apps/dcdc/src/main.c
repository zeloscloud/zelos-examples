/*
 * DC-DC converter.
 *
 * Steps the high-voltage pack down to the 12 V rail. The pack current it draws
 * is bounded by BMS_Limits.AuxCurrentLimit, which the BMS lowers while the
 * vehicle charges.
 *
 * Draw is slew-rate limited, because snapping the input current to a new
 * setpoint stresses the pack contactors and the input filter.
 */

#include <bench.h>
#include <bench/bench_can.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dcdc, LOG_LEVEL_INF);

/* What the 12 V loads ask of the converter, referred to the pack side. */
#define DEMAND_A 4.5f

/* Rate cap on the pack-side current, amps per second. */
#define SLEW_A_PER_S 10.0f

#define RAIL_NOMINAL_V 13.8f
#define RAIL_DROOP_V_PER_A 0.0035f

/* Thermal model: rises with throughput, falls toward ambient. */
#define AMBIENT_C 28.0f
#define THERMAL_GAIN_C_PER_A 4.5f
#define THERMAL_TAU_S 6.0f
#define DERATE_ABOVE_C 70.0f

/* Conversion efficiency, used to turn pack-side current into rail current. */
#define PACK_NOMINAL_V 374.0f
#define EFFICIENCY 0.93f

K_MSGQ_DEFINE(limits_msgq, sizeof(struct can_frame), 4, 4);

static float latest_aux_limit(float current_limit_a)
{
	struct can_frame frame;
	struct bench_bms_limits_t limits;

	while (k_msgq_get(&limits_msgq, &frame, K_NO_WAIT) == 0) {
		if (bench_bms_limits_unpack(&limits, frame.data, can_dlc_to_bytes(frame.dlc)) ==
		    0) {
			current_limit_a = bench_bms_limits_aux_current_limit_decode(
				limits.aux_current_limit);
		}
	}

	return current_limit_a;
}

int main(void)
{
	const float dt_s = (float)BENCH_DCDC_STATUS_CYCLE_TIME_MS / 1000.0f;
	const float slew_step_a = SLEW_A_PER_S * dt_s;
	float input_current_a = 0.0f;
	float aux_limit_a = 0.0f;
	float temperature_c = AMBIENT_C;
	uint8_t counter = 0U;
	int64_t deadline;

	if (bench_can_init() != 0) {
		return 0;
	}

	if (bench_can_subscribe(BENCH_BMS_LIMITS_FRAME_ID, &limits_msgq) < 0) {
		LOG_ERR("subscribe to BMS_Limits failed");
		return 0;
	}

	LOG_INF("DC-DC up, %u ms cycle, slew %d A/s", BENCH_DCDC_STATUS_CYCLE_TIME_MS,
		(int)SLEW_A_PER_S);

	deadline = k_uptime_get();

	for (;;) {
		struct bench_dcdc_status_t status = {0};
		uint8_t buf[BENCH_DCDC_STATUS_LENGTH];
		float setpoint_a;
		float rail_current_a;

		aux_limit_a = latest_aux_limit(aux_limit_a);

		/* Never ask for more than the loads need, or more than the pack allows. */
		setpoint_a = MIN(DEMAND_A, aux_limit_a);

		/*
		 * Approach the setpoint. Increases are always rate-capped.
		 * Whether reductions are too is the only difference between this
		 * node's two builds; see apps/dcdc/Kconfig.
		 */
		if (setpoint_a < input_current_a) {
			if (IS_ENABLED(CONFIG_BENCH_DCDC_HONOUR_LIMIT_IMMEDIATELY)) {
				input_current_a = setpoint_a;
			} else {
				input_current_a = MAX(input_current_a - slew_step_a, setpoint_a);
			}
		} else {
			input_current_a = MIN(input_current_a + slew_step_a, setpoint_a);
		}

		rail_current_a =
			input_current_a * PACK_NOMINAL_V * EFFICIENCY / RAIL_NOMINAL_V;

		temperature_c += ((AMBIENT_C + THERMAL_GAIN_C_PER_A * input_current_a) -
				  temperature_c) *
				 (dt_s / THERMAL_TAU_S);

		status.output_voltage = bench_dcdc_status_output_voltage_encode(
			RAIL_NOMINAL_V - RAIL_DROOP_V_PER_A * rail_current_a);
		status.output_current = bench_dcdc_status_output_current_encode(rail_current_a);
		status.input_current = bench_dcdc_status_input_current_encode(input_current_a);
		status.temperature = bench_dcdc_status_temperature_encode(temperature_c);
		status.derate_active = temperature_c > DERATE_ABOVE_C
					       ? BENCH_DCDC_STATUS_DERATE_ACTIVE_ACTIVE_CHOICE
					       : BENCH_DCDC_STATUS_DERATE_ACTIVE_INACTIVE_CHOICE;
		status.counter = counter & 0x7FU;

		if (bench_dcdc_status_pack(buf, &status, sizeof(buf)) != (int)sizeof(buf)) {
			LOG_ERR("pack failed");
			return 0;
		}

		(void)bench_can_send(BENCH_DCDC_STATUS_FRAME_ID, buf, sizeof(buf));

		counter++;

		deadline += BENCH_DCDC_STATUS_CYCLE_TIME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
