/*
 * VCU — vehicle control unit.
 *
 * Commands the operating mode and carries the bus heartbeat. It walks a fixed
 * drive cycle, so every run of the bench produces the same sequence: stand
 * still, drive, charge, repeat.
 */

#include <bench.h>
#include <zelos/can.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vcu, LOG_LEVEL_INF);

/* One pass of the drive cycle, in firmware time. */
#define STANDBY_MS 2000
#define DRIVE_MS   8000
#define CHARGE_MS  5000
#define CYCLE_MS   (STANDBY_MS + DRIVE_MS + CHARGE_MS)

#define PEAK_TORQUE_NM 220.0f
#define REGEN_TORQUE_NM (-70.0f)

/* The last part of the drive phase is regenerative braking. */
#define REGEN_MS 1500

static uint8_t mode_at(uint32_t phase_ms)
{
	if (phase_ms < STANDBY_MS) {
		return BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE;
	}
	if (phase_ms < STANDBY_MS + DRIVE_MS) {
		return BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE;
	}
	return BENCH_VCU_COMMAND_REQUESTED_MODE_CHARGE_CHOICE;
}

/* A triangular accelerator trace, then a regenerative braking tail. */
static float torque_at(uint32_t phase_ms)
{
	uint32_t into_drive;
	uint32_t ramp_ms;

	if (mode_at(phase_ms) != BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE) {
		return 0.0f;
	}

	into_drive = phase_ms - STANDBY_MS;

	if (into_drive >= DRIVE_MS - REGEN_MS) {
		return REGEN_TORQUE_NM;
	}

	ramp_ms = DRIVE_MS - REGEN_MS;

	if (into_drive < ramp_ms / 2U) {
		return PEAK_TORQUE_NM * (float)into_drive / (float)(ramp_ms / 2U);
	}

	return PEAK_TORQUE_NM * (float)(ramp_ms - into_drive) / (float)(ramp_ms / 2U);
}

int main(void)
{
	struct bench_vcu_command_t cmd = {0};
	uint8_t buf[BENCH_VCU_COMMAND_LENGTH];
	int64_t deadline;
	uint8_t heartbeat = 0U;

	if (zelos_can_init() != 0) {
		return 0;
	}

	LOG_INF("VCU up, %u ms cycle, drive cycle %u ms",
		BENCH_VCU_COMMAND_CYCLE_TIME_MS, CYCLE_MS);

	deadline = k_uptime_get();

	for (;;) {
		uint32_t phase_ms = (uint32_t)(k_uptime_get() % CYCLE_MS);
		uint8_t mode = mode_at(phase_ms);

		cmd.requested_mode = mode;
		cmd.torque_request = bench_vcu_command_torque_request_encode(torque_at(phase_ms));
		cmd.heartbeat = heartbeat++;

		if (bench_vcu_command_pack(buf, &cmd, sizeof(buf)) != (int)sizeof(buf)) {
			LOG_ERR("pack failed");
			return 0;
		}

		(void)zelos_can_send(BENCH_VCU_COMMAND_FRAME_ID, buf, sizeof(buf));

		/*
		 * An absolute deadline rather than k_sleep(period): a relative
		 * sleep adds the loop's own run time to every cycle and drifts.
		 */
		deadline += BENCH_VCU_COMMAND_CYCLE_TIME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
