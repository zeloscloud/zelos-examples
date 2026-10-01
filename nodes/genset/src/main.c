/*
 * Genset — the range extender's engine ECU, a J1939 node.
 *
 * The engine follows the mode the VCU commands: it runs hard while driving,
 * idles in standby and stops while the vehicle charges from the grid. Under
 * sustained load the coolant runs hot, and the ECU derates and raises
 * diagnostic trouble codes, which go out in DM1.
 */

#include <bench.h>
#include <zelos/can.h>
#include <zelos/j1939.h>

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(genset, LOG_LEVEL_INF);

#define PGN_EEC1 61444U /* Electronic Engine Controller 1 */
#define PGN_ET1  65262U /* Engine Temperature 1 */
#define PGN_DM1  65226U /* Active Diagnostic Trouble Codes */

/* J1939-71 ties EEC1's rate to engine speed, tens of ms; 100 ms keeps the bench bus light. */
#define EEC1_PERIOD_MS 100U
#define ET1_PERIOD_MS  1000U
#define DM1_PERIOD_MS  1000U

/* A self-configurable address, so the NAME says arbitrary-address capable. */
#define PREFERRED_ADDRESS 0x80U

/* Function 0 is Engine. Manufacturer 0 and the identity are examples, not a product. */
#define NAME ZELOS_J1939_NAME(1, 0, 0, 0, 0, 0, 0, 0, 0x0A11)

#define LOOP_MS 100U

/* The VCU sends every 100 ms; after this long without it, fall back to standby. */
#define VCU_TIMEOUT_MS 500

#define IDLE_RPM 900.0f
#define RUN_RPM  2400.0f
#define RAMP_RPM_PER_S 600.0f

/* Actual engine percent torque by mode, and the cap while derating. */
#define IDLE_TORQUE_PCT   8.0f
#define RUN_TORQUE_PCT    85.0f
#define DERATE_TORQUE_PCT 60.0f

/*
 * Coolant: thermostat-held near 88 degC, higher with load, back to ambient
 * when stopped. The time constant is compressed so one drive phase of the
 * bench shows the whole story.
 */
#define AMBIENT_C 25.0f
#define THERMOSTAT_C 88.0f
#define LOAD_GAIN_C 35.0f
#define COOLANT_TAU_S 4.0f
#define HOT_ON_C 105.0f
#define HOT_OFF_C 100.0f

/* DTCs raised while hot: SPN, FMI. */
#define SPN_COOLANT_TEMP 110U  /* FMI 16: above normal, moderately severe */
#define SPN_TORQUE_DERATE 1569U /* Engine Protection Torque Derate; FMI 31: condition exists */
#define FMI_HIGH_MODERATE 16U
#define FMI_CONDITION_EXISTS 31U

/* DM1 byte 1: amber warning lamp on (bits 4-3 = 01), the rest off. */
#define DM1_LAMP_AMBER 0x04U

K_MSGQ_DEFINE(vcu_msgq, sizeof(struct can_frame), 4, 4);

static uint8_t eec1[8];
static uint8_t et1[8];
static uint8_t dm1[ZELOS_J1939_MAX_LEN];

static struct zelos_j1939_msg msgs[] = {
	{.pgn = PGN_EEC1, .priority = 3, .period_ms = EEC1_PERIOD_MS, .len = 8, .data = eec1},
	{.pgn = PGN_ET1, .priority = 6, .period_ms = ET1_PERIOD_MS, .len = 8, .data = et1},
	{.pgn = PGN_DM1, .priority = 6, .period_ms = DM1_PERIOD_MS, .len = 8, .data = dm1},
};

static struct zelos_j1939 j1939 = {
	.name = NAME,
	.preferred = PREFERRED_ADDRESS,
	.msgs = msgs,
	.n_msgs = ARRAY_SIZE(msgs),
};

struct engine {
	uint8_t mode;
	int64_t last_command_ms;
	float rpm;
	float torque_pct;
	float coolant_c;
	bool hot;
	uint8_t occurrences;
};

static void follow_vcu(struct engine *e, int64_t now_ms)
{
	struct can_frame frame;
	struct bench_vcu_command_t cmd;

	while (k_msgq_get(&vcu_msgq, &frame, K_NO_WAIT) == 0) {
		if (bench_vcu_command_unpack(&cmd, frame.data, can_dlc_to_bytes(frame.dlc)) == 0) {
			e->mode = cmd.requested_mode;
			e->last_command_ms = now_ms;
		}
	}

	if (now_ms - e->last_command_ms > VCU_TIMEOUT_MS) {
		e->mode = BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE;
	}
}

static void step(struct engine *e)
{
	const float dt_s = (float)LOOP_MS / 1000.0f;
	float target_rpm = 0.0f;
	float target_c;
	float load;

	e->torque_pct = 0.0f;
	switch (e->mode) {
	case BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE:
		target_rpm = RUN_RPM;
		e->torque_pct = e->hot ? DERATE_TORQUE_PCT : RUN_TORQUE_PCT;
		break;
	case BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE:
		target_rpm = IDLE_RPM;
		e->torque_pct = IDLE_TORQUE_PCT;
		break;
	default:
		/* Charging from the grid, or a fault: stopped. */
		break;
	}

	if (e->rpm < target_rpm) {
		e->rpm = MIN(e->rpm + RAMP_RPM_PER_S * dt_s, target_rpm);
	} else {
		e->rpm = MAX(e->rpm - RAMP_RPM_PER_S * dt_s, target_rpm);
	}

	load = (e->torque_pct / 100.0f) * (e->rpm / RUN_RPM);
	target_c = e->rpm > 0.0f ? THERMOSTAT_C + LOAD_GAIN_C * load : AMBIENT_C;
	e->coolant_c += (target_c - e->coolant_c) * (dt_s / COOLANT_TAU_S);

	if (!e->hot && e->coolant_c > HOT_ON_C) {
		e->hot = true;
		e->occurrences = MIN(e->occurrences + 1, 126);
		LOG_WRN("coolant %d degC: derating", (int)e->coolant_c);
	} else if (e->hot && e->coolant_c < HOT_OFF_C) {
		e->hot = false;
		LOG_INF("coolant %d degC: derate cleared", (int)e->coolant_c);
	}
}

/*
 * SPN 513 Actual Engine Percent Torque: byte 3, 1 %/bit, -125 %.
 * SPN 190 Engine Speed: bytes 4-5, 0.125 rpm/bit.
 */
static void encode_eec1(const struct engine *e, uint8_t *d)
{
	uint16_t speed = (uint16_t)(e->rpm / 0.125f);

	memset(d, 0xFF, 8);
	d[2] = (uint8_t)(e->torque_pct + 125.0f);
	d[3] = speed & 0xFFU;
	d[4] = speed >> 8;
}

/* SPN 110 Engine Coolant Temperature, byte 1, 1 degC/bit, -40 degC. */
static void encode_et1(const struct engine *e, uint8_t *d)
{
	memset(d, 0xFF, 8);
	d[0] = (uint8_t)CLAMP(e->coolant_c + 40.0f, 0.0f, 250.0f);
}

/* J1939-73 DTC: SPN low 16 bits, then SPN high 3 bits over the FMI, then occurrence count. */
static void put_dtc(uint8_t *p, uint32_t spn, uint8_t fmi, uint8_t occurrences)
{
	p[0] = spn & 0xFFU;
	p[1] = (spn >> 8) & 0xFFU;
	p[2] = (uint8_t)(((spn >> 16) & 0x7U) << 5) | (fmi & 0x1FU);
	p[3] = occurrences & 0x7FU;
}

/* Two DTCs make 10 bytes, so a hot engine's DM1 goes by BAM. */
static uint16_t encode_dm1(const struct engine *e, uint8_t *d)
{
	static const uint8_t none[8] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};

	if (!e->hot) {
		memcpy(d, none, sizeof(none));
		return sizeof(none);
	}

	d[0] = DM1_LAMP_AMBER;
	d[1] = 0xFF;
	put_dtc(&d[2], SPN_COOLANT_TEMP, FMI_HIGH_MODERATE, e->occurrences);
	put_dtc(&d[6], SPN_TORQUE_DERATE, FMI_CONDITION_EXISTS, e->occurrences);
	return 10;
}

static void publish(const struct engine *e)
{
	uint8_t buf[ZELOS_J1939_MAX_LEN];
	uint16_t len;

	encode_eec1(e, buf);
	zelos_j1939_set(&msgs[0], buf, 8);
	encode_et1(e, buf);
	zelos_j1939_set(&msgs[1], buf, 8);
	len = encode_dm1(e, buf);
	zelos_j1939_set(&msgs[2], buf, len);
}

int main(void)
{
	struct engine engine = {
		.mode = BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE,
		.coolant_c = AMBIENT_C,
	};
	int64_t deadline;

	if (zelos_can_init() != 0) {
		return 0;
	}

	if (zelos_can_subscribe(BENCH_VCU_COMMAND_FRAME_ID, &vcu_msgq) < 0) {
		LOG_ERR("subscribe to VCU_Command failed");
		return 0;
	}

	publish(&engine);
	if (zelos_j1939_start(&j1939) != 0) {
		return 0;
	}

	LOG_INF("genset up, NAME 0x%016llx", (unsigned long long)NAME);

	deadline = k_uptime_get();
	engine.last_command_ms = deadline;

	for (;;) {
		follow_vcu(&engine, k_uptime_get());
		step(&engine);
		publish(&engine);

		deadline += LOOP_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
