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
#include <zelos/can.h>

#include "calibration.h"
#include "dcdc_node.h"

#include <string.h>

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

#if defined(CONFIG_ZELOS_XCP) || defined(CONFIG_ZELOS_UDS)
/*
 * Built with `-S xcp` or `-S uds`, a tool may recalibrate the demand and slew
 * rate while the node runs, so they move from constants into RAM.
 */

struct dcdc_calibration dcdc_cal = {
	.demand_a = DEMAND_A,
	.slew_a_per_s = SLEW_A_PER_S,
};

/* Around every write to dcdc_cal and the model's read of it. */
static struct k_spinlock dcdc_cal_lock;

void dcdc_cal_write(void *dst, const void *src, size_t len)
{
	K_SPINLOCK(&dcdc_cal_lock) {
		memcpy(dst, src, len);
	}
}

/*
 * The node bounds what it was sent rather than trusting the tool to honour its
 * limits. Written so that NaN lands on the lower bound.
 */
static float bounded(float value, float lo, float hi)
{
	return !(value >= lo) ? lo : (value > hi ? hi : value);
}
#endif

#if defined(CONFIG_ZELOS_XCP)
#include "xcp.h"
#endif

#if defined(CONFIG_ZELOS_UDS) || defined(CONFIG_ZELOS_AUTOSAR)
/*
 * Built with `-S uds` or `-S autosar`, the node reports each cycle's state,
 * the pack voltage it sees among it. See src/state.h.
 */
#define DCDC_STATE

#include <math.h>

#include "state.h"

/* A BMS frame older than this is missing. */
#define BMS_TIMEOUT_MS (5 * BENCH_BMS_LIMITS_CYCLE_TIME_MS)

K_MSGQ_DEFINE(dcdc_pack_msgq, sizeof(struct can_frame), 4, 4);

static int64_t pack_at_ms;
static float pack_v;

/* The latest BMS_Status.PackVoltage; NaN when there is none fresh. */
static float latest_pack_v(bool apply, int64_t now)
{
	struct can_frame frame;
	struct bench_bms_status_t pack;

	while (k_msgq_get(&dcdc_pack_msgq, &frame, K_NO_WAIT) == 0) {
		if (apply && bench_bms_status_unpack(&pack, frame.data,
						     can_dlc_to_bytes(frame.dlc)) == 0) {
			pack_v = bench_bms_status_pack_voltage_decode(pack.pack_voltage);
			pack_at_ms = now;
		}
	}
	return pack_at_ms != 0 && now - pack_at_ms <= BMS_TIMEOUT_MS ? pack_v : NAN;
}
#endif

#if defined(CONFIG_ZELOS_UDS)
/*
 * Built with `-S uds`, a tester sets the output voltage, controls the node's
 * communication and reads its state and DTCs. See src/uds.c.
 */
#include <zelos/uds.h>

#include "uds.h"

static int64_t limits_at_ms;
#endif

#if defined(CONFIG_ZELOS_AUTOSAR)
/* Built with `-S autosar`, the node sends its AUTOSAR PDUs. See src/autosar.c. */
#include "autosar.h"
#endif

/* Demand and slew rate, as one pair: a tool's write lands whole or not yet. */
static struct dcdc_calibration calibration(void)
{
#if defined(CONFIG_ZELOS_XCP) || defined(CONFIG_ZELOS_UDS)
	struct dcdc_calibration cal;

	K_SPINLOCK(&dcdc_cal_lock) {
		cal = dcdc_cal;
	}
	cal.demand_a = bounded(cal.demand_a, 0.0f, 20.0f);
	cal.slew_a_per_s = bounded(cal.slew_a_per_s, 0.1f, 100.0f);
	return cal;
#else
	return (struct dcdc_calibration){.demand_a = DEMAND_A, .slew_a_per_s = SLEW_A_PER_S};
#endif
}

/* The rail's no-load voltage. */
static float rail_nominal_v(void)
{
#if defined(CONFIG_ZELOS_UDS)
	return dcdc_uds_output_v();
#else
	return RAIL_NOMINAL_V;
#endif
}

K_MSGQ_DEFINE(dcdc_limits_msgq, sizeof(struct can_frame), 4, 4);

/* Frames are drained either way; applied only while reception is enabled. */
static float latest_aux_limit(float current_limit_a, bool apply)
{
	struct can_frame frame;
	struct bench_bms_limits_t limits;

	while (k_msgq_get(&dcdc_limits_msgq, &frame, K_NO_WAIT) == 0) {
		if (apply && bench_bms_limits_unpack(&limits, frame.data,
						     can_dlc_to_bytes(frame.dlc)) == 0) {
			current_limit_a = bench_bms_limits_aux_current_limit_decode(
				limits.aux_current_limit);
#if defined(CONFIG_ZELOS_UDS)
			limits_at_ms = k_uptime_get();
#endif
		}
	}

	return current_limit_a;
}

static void run(void *p1, void *p2, void *p3);

/*
 * The control loop, on a thread of its own so an image can run it beside other
 * nodes; main()'s priority and stack, where it ran before.
 */
K_THREAD_DEFINE(dcdc_tid, 1024, run, NULL, NULL, NULL, 0, 0, SYS_FOREVER_MS);

int dcdc_start(void)
{
	if (zelos_can_subscribe(BENCH_BMS_LIMITS_FRAME_ID, &dcdc_limits_msgq) < 0) {
		LOG_ERR("subscribe to BMS_Limits failed");
		return -EIO;
	}

#if defined(DCDC_STATE)
	/* The pack voltage: the input undervoltage DTC, the AUTOSAR PDUs. */
	if (zelos_can_subscribe(BENCH_BMS_STATUS_FRAME_ID, &dcdc_pack_msgq) < 0) {
		LOG_ERR("subscribe to BMS_Status failed");
		return -EIO;
	}
#endif

#if defined(CONFIG_ZELOS_XCP)
	if (dcdc_xcp_start() != 0) {
		return -EIO;
	}
#endif

#if defined(CONFIG_ZELOS_UDS)
	if (dcdc_uds_start() != 0) {
		return -EIO;
	}
#endif

#if defined(CONFIG_ZELOS_AUTOSAR)
	if (dcdc_autosar_start() != 0) {
		return -EIO;
	}
#endif

	LOG_INF("DC-DC up, %u ms cycle, slew %d A/s", BENCH_DCDC_STATUS_CYCLE_TIME_MS,
		(int)calibration().slew_a_per_s);

	k_thread_start(dcdc_tid);

	return 0;
}

static void run(void *p1, void *p2, void *p3)
{
	const float dt_s = (float)BENCH_DCDC_STATUS_CYCLE_TIME_MS / 1000.0f;
	float input_current_a = 0.0f;
	float aux_limit_a = 0.0f;
	float temperature_c = AMBIENT_C;
	uint8_t counter = 0U;
	int64_t deadline;
#if defined(CONFIG_ZELOS_UDS)
	bool rx_was = true;
#endif

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	deadline = k_uptime_get();

	for (;;) {
		struct bench_dcdc_status_t status = {0};
		uint8_t buf[BENCH_DCDC_STATUS_LENGTH];
		/* Per cycle, because XCP and UDS can recalibrate both. */
		const struct dcdc_calibration cal = calibration();
		const float slew_step_a = cal.slew_a_per_s * dt_s;
		float setpoint_a;
		float rail_current_a;
		float rail_voltage_v;
#if defined(CONFIG_ZELOS_UDS)
		/* CommunicationControl: the application's normal messages only. */
		const bool rx = zelos_uds_rx_enabled();
		const bool tx = zelos_uds_tx_enabled();
#else
		const bool rx = true;
		const bool tx = true;
#endif
#if defined(DCDC_STATE)
		const int64_t now = k_uptime_get();
		const float input_v = latest_pack_v(rx, now);
#endif
#if defined(CONFIG_ZELOS_UDS)
		/* Reception back on: the BMS timeout runs from here, not the last frame before. */
		if (rx && !rx_was) {
			limits_at_ms = now;
		}
		rx_was = rx;
#endif

		aux_limit_a = latest_aux_limit(aux_limit_a, rx);

		/* Never ask for more than the loads need, or more than the pack allows. */
		setpoint_a = MIN(cal.demand_a, aux_limit_a);

		/*
		 * Approach the setpoint. Increases are always rate-capped.
		 * Whether reductions are too is the only difference between this
		 * node's two builds; see nodes/dcdc/Kconfig.
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
			input_current_a * PACK_NOMINAL_V * EFFICIENCY / rail_nominal_v();

		temperature_c += ((AMBIENT_C + THERMAL_GAIN_C_PER_A * input_current_a) -
				  temperature_c) *
				 (dt_s / THERMAL_TAU_S);

		rail_voltage_v = rail_nominal_v() - RAIL_DROOP_V_PER_A * rail_current_a;

#if defined(CONFIG_ZELOS_XCP)
		dcdc_xcp_sample(setpoint_a, input_current_a, slew_step_a, temperature_c,
				aux_limit_a);
#endif
#if defined(DCDC_STATE)
		const struct dcdc_state state = {
			.output_v = rail_voltage_v,
			.output_a = rail_current_a,
			.input_v = input_v,
			.input_a = input_current_a,
			.temperature_c = temperature_c,
			.setpoint_v = rail_nominal_v(),
			.aux_limit_a = aux_limit_a,
			.uptime_ms = (uint32_t)now,
		};
#endif
#if defined(CONFIG_ZELOS_UDS)
		dcdc_uds_sample(&state,
				/*
				 * Monitored while reception is on, from the first
				 * frame or the first timeout, whichever comes first.
				 */
				rx && (limits_at_ms != 0 || now > BMS_TIMEOUT_MS),
				now - limits_at_ms > BMS_TIMEOUT_MS);
#endif
#if defined(CONFIG_ZELOS_AUTOSAR)
		dcdc_autosar_sample(&state, temperature_c > DERATE_ABOVE_C, tx);
#endif

		status.output_voltage = bench_dcdc_status_output_voltage_encode(rail_voltage_v);
		status.output_current = bench_dcdc_status_output_current_encode(rail_current_a);
		status.input_current = bench_dcdc_status_input_current_encode(input_current_a);
		status.temperature = bench_dcdc_status_temperature_encode(temperature_c);
		status.derate_active = temperature_c > DERATE_ABOVE_C
					       ? BENCH_DCDC_STATUS_DERATE_ACTIVE_ACTIVE_CHOICE
					       : BENCH_DCDC_STATUS_DERATE_ACTIVE_INACTIVE_CHOICE;
		status.counter = counter & 0x7FU;

		if (bench_dcdc_status_pack(buf, &status, sizeof(buf)) != (int)sizeof(buf)) {
			LOG_ERR("pack failed");
			return;
		}

		/* The counter counts frames sent, so a gap in it is a reset. */
		if (tx) {
			(void)zelos_can_send(BENCH_DCDC_STATUS_FRAME_ID, buf, sizeof(buf));
			counter++;
		}

		deadline += BENCH_DCDC_STATUS_CYCLE_TIME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}
