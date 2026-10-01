/*
 * BMS — battery management system.
 *
 * Reports pack state and publishes the current envelope the rest of the bus
 * must respect. The envelope depends on the mode the VCU commands, so entering
 * Charge lowers the DC-DC's allowance sharply.
 */

#include <bench.h>
#include <zelos/can.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bms, LOG_LEVEL_INF);

#define CELL_COUNT 96

/* Current the DC-DC may draw from the pack, by mode. Charging leaves least. */
#define AUX_LIMIT_STANDBY_A 3.0f
#define AUX_LIMIT_DRIVE_A   6.0f
#define AUX_LIMIT_CHARGE_A  2.0f

#define DISCHARGE_LIMIT_DRIVE_A   180.0f
#define DISCHARGE_LIMIT_STANDBY_A  20.0f
#define CHARGE_LIMIT_A             64.0f

/* Cell 4 sits slightly low, like the weakest cell in a real pack. */
#define WEAK_CELL_DROP_V 0.045f

/* BMS_CellVoltages is slower than the status frames. */
#define CELLS_EVERY (BENCH_BMS_CELL_VOLTAGES_CYCLE_TIME_MS / BENCH_BMS_STATUS_CYCLE_TIME_MS)

K_MSGQ_DEFINE(vcu_msgq, sizeof(struct can_frame), 4, 4);

struct pack_state {
	float soc_pct;
	float cell_v;
	uint8_t mode;
};

static void follow_vcu(struct pack_state *pack)
{
	struct can_frame frame;
	struct bench_vcu_command_t cmd;

	/* Drain the queue: the newest command is the current one. */
	while (k_msgq_get(&vcu_msgq, &frame, K_NO_WAIT) == 0) {
		if (bench_vcu_command_unpack(&cmd, frame.data, can_dlc_to_bytes(frame.dlc)) == 0) {
			pack->mode = cmd.requested_mode;
		}
	}
}

static float aux_limit_for(uint8_t mode)
{
	switch (mode) {
	case BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE:
		return AUX_LIMIT_DRIVE_A;
	case BENCH_VCU_COMMAND_REQUESTED_MODE_CHARGE_CHOICE:
		return AUX_LIMIT_CHARGE_A;
	case BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE:
		return AUX_LIMIT_STANDBY_A;
	default:
		return 0.0f;
	}
}

/* Integrate pack current into state of charge, and sag the cells under load. */
static void step_pack(struct pack_state *pack, float pack_current_a)
{
	const float dt_h = (float)BENCH_BMS_STATUS_CYCLE_TIME_MS / 3600000.0f;
	const float capacity_ah = 120.0f;

	pack->soc_pct -= (pack_current_a * dt_h / capacity_ah) * 100.0f;
	pack->soc_pct = CLAMP(pack->soc_pct, 5.0f, 100.0f);

	/* 3.3 V empty to 4.1 V full, minus a little internal-resistance sag. */
	pack->cell_v = 3.3f + 0.008f * pack->soc_pct - 0.0004f * pack_current_a;
}

static float pack_current_for(uint8_t mode, float soc_pct)
{
	switch (mode) {
	case BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE:
		return 60.0f + soc_pct * 0.3f;
	case BENCH_VCU_COMMAND_REQUESTED_MODE_CHARGE_CHOICE:
		return -CHARGE_LIMIT_A;
	default:
		return 1.5f;
	}
}

int main(void)
{
	struct pack_state pack = {
		.soc_pct = 78.0f,
		.cell_v = 3.9f,
		.mode = BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE,
	};
	uint8_t counter = 0U;
	uint32_t tick = 0U;
	int64_t deadline;

	if (zelos_can_init() != 0) {
		return 0;
	}

	if (zelos_can_subscribe(BENCH_VCU_COMMAND_FRAME_ID, &vcu_msgq) < 0) {
		LOG_ERR("subscribe to VCU_Command failed");
		return 0;
	}

	LOG_INF("BMS up, %u cells, %u ms status cycle", CELL_COUNT,
		BENCH_BMS_STATUS_CYCLE_TIME_MS);

	deadline = k_uptime_get();

	for (;;) {
		struct bench_bms_status_t status = {0};
		struct bench_bms_limits_t limits = {0};
		uint8_t status_buf[BENCH_BMS_STATUS_LENGTH];
		uint8_t limits_buf[BENCH_BMS_LIMITS_LENGTH];
		float pack_current_a;
		bool driving;

		follow_vcu(&pack);
		pack_current_a = pack_current_for(pack.mode, pack.soc_pct);
		step_pack(&pack, pack_current_a);

		driving = pack.mode != BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE;

		status.pack_voltage =
			bench_bms_status_pack_voltage_encode(pack.cell_v * (float)CELL_COUNT);
		status.pack_current = bench_bms_status_pack_current_encode(pack_current_a);
		status.state_of_charge = bench_bms_status_state_of_charge_encode(pack.soc_pct);
		status.contactor_state = driving ? BENCH_BMS_STATUS_CONTACTOR_STATE_CLOSED_CHOICE
						 : BENCH_BMS_STATUS_CONTACTOR_STATE_OPEN_CHOICE;
		status.counter = counter;

		limits.discharge_current_limit = bench_bms_limits_discharge_current_limit_encode(
			pack.mode == BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE
				? DISCHARGE_LIMIT_DRIVE_A
				: DISCHARGE_LIMIT_STANDBY_A);
		limits.charge_current_limit = bench_bms_limits_charge_current_limit_encode(
			pack.mode == BENCH_VCU_COMMAND_REQUESTED_MODE_CHARGE_CHOICE ? CHARGE_LIMIT_A
										   : 0.0f);
		limits.aux_current_limit =
			bench_bms_limits_aux_current_limit_encode(aux_limit_for(pack.mode));
		limits.counter = counter;

		if (bench_bms_status_pack(status_buf, &status, sizeof(status_buf)) ==
			    (int)sizeof(status_buf) &&
		    bench_bms_limits_pack(limits_buf, &limits, sizeof(limits_buf)) ==
			    (int)sizeof(limits_buf)) {
			(void)zelos_can_send(BENCH_BMS_STATUS_FRAME_ID, status_buf,
					     sizeof(status_buf));
			(void)zelos_can_send(BENCH_BMS_LIMITS_FRAME_ID, limits_buf,
					     sizeof(limits_buf));
		} else {
			LOG_ERR("pack failed");
			return 0;
		}

		if ((tick % CELLS_EVERY) == 0U) {
			struct bench_bms_cell_voltages_t cells = {0};
			uint8_t cells_buf[BENCH_BMS_CELL_VOLTAGES_LENGTH];

			cells.cell_voltage1 =
				bench_bms_cell_voltages_cell_voltage1_encode(pack.cell_v + 0.004f);
			cells.cell_voltage2 =
				bench_bms_cell_voltages_cell_voltage2_encode(pack.cell_v);
			cells.cell_voltage3 =
				bench_bms_cell_voltages_cell_voltage3_encode(pack.cell_v + 0.007f);
			cells.cell_voltage4 = bench_bms_cell_voltages_cell_voltage4_encode(
				pack.cell_v - WEAK_CELL_DROP_V);
			cells.counter = counter;

			if (bench_bms_cell_voltages_pack(cells_buf, &cells, sizeof(cells_buf)) ==
			    (int)sizeof(cells_buf)) {
				(void)zelos_can_send(BENCH_BMS_CELL_VOLTAGES_FRAME_ID, cells_buf,
						     sizeof(cells_buf));
			}
		}

		counter++;
		tick++;

		deadline += BENCH_BMS_STATUS_CYCLE_TIME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
