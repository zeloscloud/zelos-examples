/*
 * Genset — the bench's engine and generator ECU, a J1939 node.
 *
 * The engine follows the mode the VCU commands: rated speed under load while
 * driving, idle in standby, stopped while the vehicle charges from the grid.
 * The generator rides on it, a 4-pole machine: 60 Hz at 1800 rpm, 480 V
 * line-line, 100 kW rated, power factor 0.8 lagging. Current and power follow
 * the load; energy, fuel and hours integrate. The values across the PGs come
 * from one model state, so a decoder can cross-check them.
 *
 * Under sustained load the coolant runs hot: the ECU derates and raises
 * SPN 110 in DM1, then the thinning oil loses pressure and SPN 100 joins it,
 * so DM1 outgrows a frame and goes by BAM. DM2 lists what was active before,
 * on request only, like SOFT: by RTS/CTS to the requester, by BAM to everyone.
 */

#include <bench.h>
#include <genset.h>
#include <zelos/can.h>
#include <zelos/j1939.h>

#include "genset_node.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(genset, LOG_LEVEL_INF);

/* A self-configurable address, so the NAME says arbitrary-address capable. */
#define PREFERRED_ADDRESS 0x80U

/* Function 0 is Engine. Manufacturer 0 and the identity are examples, not a product. */
#define NAME ZELOS_J1939_NAME(1, 0, 0, 0, 0, 0, 0, 0, 0x0A11)

/* EEC2 is the fastest PG, every 50 ms; each send carries a fresh model step. */
#define LOOP_MS 50U

/* The VCU sends every 100 ms; after this long without it, fall back to standby. */
#define VCU_TIMEOUT_MS 500

#define IDLE_RPM       900.0f
#define RATED_RPM      1800.0f /* 60 Hz from four poles */
#define RAMP_RPM_PER_S 600.0f

/* Actual engine percent torque by mode, and the cap while derating. */
#define IDLE_TORQUE_PCT   8.0f
#define RUN_TORQUE_PCT    85.0f
#define DERATE_TORQUE_PCT 60.0f

/* Generator at rated speed. Voltage follows speed, power follows torque and speed. */
#define V_LN_RATED   277.0f
#define RATED_W      100000.0f
#define POWER_FACTOR 0.8f
#define PF_LAGGING   1U /* SPN 2518: 1 lagging */
#define SQRT3        1.7320508f

/* Fuel: 1.5 L/h at idle, 0.25 L per kWh on top. */
#define IDLE_LPH 1.5f
#define LPKWH    0.25f

/*
 * Coolant: thermostat-held near 88 degC, higher with load, back to ambient
 * when stopped. The time constant is compressed so one drive phase of the
 * VCU's cycle shows the whole story: hot after 3 to 4 s, oil low 2 to 3 s later.
 */
#define AMBIENT_C     25.0f
#define THERMOSTAT_C  88.0f
#define LOAD_GAIN_C   40.0f
#define COOLANT_TAU_S 2.5f
#define HOT_ON_C      105.0f
#define HOT_OFF_C     100.0f

/* Oil: runs 8 degC over the coolant, lagging it; thins from 98 degC and loses pressure. */
#define OIL_RISE_C        8.0f
#define OIL_TAU_S         2.0f
#define OIL_THIN_C        98.0f
#define OIL_THIN_SPAN_C   15.0f
#define OIL_THIN_LOSS     0.6f
#define OIL_KPA_IDLE      150.0f
#define OIL_KPA_PER_RATED 250.0f
#define OIL_LOW_KPA       200.0f
#define OIL_OK_KPA        240.0f

/* Pressures and levels with no model of their own. */
#define FUEL_KPA         300.0f
#define OIL_LEVEL_PCT    80.0f
#define COOLANT_LVL_PCT  95.0f
#define BARO_KPA         101.0f
#define BATTERY_V        25.2f
#define CHARGING_V       27.6f
#define ALTERNATOR_V     28.0f
#define ALTERNATOR_A     20.0f
#define NET_CHARGE_A     5.0f
#define NET_DISCHARGE_A  (-8.0f)

/*
 * The counters start where a unit 1000 h old would be, at rated load: an ECU
 * keeps them in non-volatile memory, which this bench has none of.
 */
#define HISTORY_H 1000ULL
#define HISTORY_W 85000ULL
#define HISTORY_ML_PER_H 22750ULL /* IDLE_LPH + LPKWH * 85 kW, in mL */

/* DM1 lamps, J1939-73 byte 1: amber warning bits 3-4, red stop bits 5-6. */
enum lamp {
	LAMP_AMBER,
	LAMP_RED,
};

/* J1939-73 DTC with the lamp it lights. */
struct dtc {
	uint32_t spn;
	uint8_t fmi;
	enum lamp lamp;
	/* Occurrence count, J1939-73: times it became active, from one earlier run. */
	uint8_t oc;
	bool active;
};

/* Coolant temperature and oil pressure, each FMI "most severe". */
static struct dtc dtcs[] = {
	{.spn = 110, .fmi = 0, .lamp = LAMP_AMBER, .oc = 1},
	{.spn = 100, .fmi = 1, .lamp = LAMP_RED, .oc = 1},
};
#define DTC_COOLANT 0
#define DTC_OIL     1

K_MSGQ_DEFINE(genset_vcu_msgq, sizeof(struct can_frame), 4, 4);

/*
 * The node's messages. The codec packs the first N_PG, 8 bytes each; DM1 and
 * DM2 grow with their DTCs; SOFT is text.
 */
enum msg {
	M_EEC1,
	M_EEC2,
	M_ET1,
	M_EFLP1,
	M_VEP1,
	M_HOURS,
	M_LFC,
	M_LFE,
	M_AMB,
	M_IC1,
	M_GAAC,
	M_GPAAC,
	M_GPBAC,
	M_GPCAC,
	M_GTACP,
	M_GTACR,
	M_GTACE,
	N_PG,
	M_DM1 = N_PG,
	M_DM2,
	M_SOFT,
	N_MSGS,
};

static uint8_t pg[N_PG][8];
static uint8_t dm1[ZELOS_J1939_MAX_LEN];
static uint8_t dm2[ZELOS_J1939_MAX_LEN];
/* J1939-71 SOFT: the number of fields, then each field ending in '*'. */
static uint8_t soft[] = "\x03" "ZELOS-GENSET*" "v1.1.0*" "zelos-examples node genset*";
#define SOFT_LEN (sizeof(soft) - 1U)
BUILD_ASSERT(SOFT_LEN > 8U && SOFT_LEN <= ZELOS_J1939_MAX_LEN, "SOFT goes by transport");

/* dbc/genset.dbc is the one list: PGN and priority from the frame id, period from the cycle time. */
#define PGN_OF(id)  (((id) >> 8) & 0x3FFFFU)
#define PRIO_OF(id) (((id) >> 26) & 7U)
#define PG(m, name, offset)                                                                    \
	[m] = {.pgn = PGN_OF(GENSET_##name##_FRAME_ID), .priority = PRIO_OF(GENSET_##name##_FRAME_ID), \
	       .period_ms = GENSET_##name##_CYCLE_TIME_MS, .offset_ms = offset, .len = 8,          \
	       .data = pg[m]}

/*
 * Offsets spread the periodic sends over the 10 ms ticks: the eight 100 ms
 * PGs one per tick, EEC2 (50 ms) at 30 and 80 ms of each 100, the slower ones
 * at 90 in turn, so no tick carries more than two periodic frames. The bus
 * sees about 111 frames a second.
 */
static struct zelos_j1939_msg msgs[N_MSGS] = {
	PG(M_EEC1, EEC1, 0),
	PG(M_LFE, LFE, 10),
	PG(M_GAAC, GAAC, 20),
	PG(M_GPAAC, GPAAC, 30),
	PG(M_GPBAC, GPBAC, 40),
	PG(M_GPCAC, GPCAC, 50),
	PG(M_GTACP, GTACP, 60),
	PG(M_GTACR, GTACR, 70),
	PG(M_EEC2, EEC2, 30),
	PG(M_EFLP1, EFL_P1, 90),
	PG(M_IC1, IC1, 190),
	PG(M_ET1, ET1, 290),
	PG(M_VEP1, VEP1, 390),
	PG(M_HOURS, HOURS, 490),
	PG(M_LFC, LFC, 590),
	PG(M_AMB, AMB, 690),
	PG(M_GTACE, GTACE, 790),
	[M_DM1] = {.pgn = PGN_OF(GENSET_DM1_FRAME_ID), .priority = PRIO_OF(GENSET_DM1_FRAME_ID),
		   .period_ms = GENSET_DM1_CYCLE_TIME_MS, .offset_ms = 890, .len = 8, .data = dm1},
	[M_DM2] = {.pgn = PGN_OF(GENSET_DM2_FRAME_ID), .priority = PRIO_OF(GENSET_DM2_FRAME_ID),
		   .period_ms = 0, .len = 8, .data = dm2},
	[M_SOFT] = {.pgn = PGN_OF(GENSET_SOFT_FRAME_ID), .priority = PRIO_OF(GENSET_SOFT_FRAME_ID),
		    .period_ms = 0, .len = SOFT_LEN, .data = soft},
};

/* The bits of each PG the DBC leaves out, sent as ones: not available. */
static const uint8_t na[N_PG][8] = {
	[M_EEC1] = {0xFF, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF},
	[M_EEC2] = {0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	[M_ET1] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF},
	[M_EFLP1] = {0, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0},
	[M_VEP1] = {0, 0, 0, 0, 0, 0, 0xFF, 0xFF},
	[M_LFE] = {0, 0, 0, 0, 0xFF, 0xFF, 0, 0xFF},
	[M_AMB] = {0, 0xFF, 0xFF, 0, 0, 0, 0xFF, 0xFF},
	[M_IC1] = {0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	[M_GTACR] = {0, 0, 0, 0, 0, 0, 0xFC, 0xFF},
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
	float oil_c;
	float oil_kpa;
	bool hot;
	/* Generator, from the speed and torque of the same step. */
	float hz;
	float v_ln;
	float i_a;
	float real_w;
	float apparent_va;
	float reactive_var;
	float fuel_lph;
	/* Integrals: whole units, with the fraction carried in float. */
	uint64_t run_ms;
	uint64_t revolutions;
	uint64_t wh;
	uint64_t fuel_ml;
	uint64_t trip_ml;
	float rev_frac;
	float wh_frac;
	float ml_frac;
};

static void follow_vcu(struct engine *e, int64_t now_ms)
{
	struct can_frame frame;
	struct bench_vcu_command_t cmd;

	while (k_msgq_get(&genset_vcu_msgq, &frame, K_NO_WAIT) == 0) {
		if (bench_vcu_command_unpack(&cmd, frame.data, can_dlc_to_bytes(frame.dlc)) == 0) {
			e->mode = cmd.requested_mode;
			e->last_command_ms = now_ms;
		}
	}

	if (now_ms - e->last_command_ms > VCU_TIMEOUT_MS) {
		e->mode = BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE;
	}
}

/* Move the whole units out of a running fraction. */
static uint32_t carry(float *frac)
{
	uint32_t whole = (uint32_t)*frac;

	*frac -= (float)whole;
	return whole;
}

static void dtc_on(struct dtc *d, const char *what)
{
	if (!d->active) {
		d->active = true;
		d->oc = MIN(d->oc + 1, 126);
		LOG_WRN("%s: SPN %u FMI %u, occurrence %u", what, d->spn, d->fmi, d->oc);
	}
}

static void dtc_off(struct dtc *d, const char *what)
{
	if (d->active) {
		d->active = false;
		LOG_INF("%s cleared: SPN %u", what, d->spn);
	}
}

static void step(struct engine *e)
{
	const float dt_s = (float)LOOP_MS / 1000.0f;
	float target_rpm = 0.0f;
	float target_c;
	float load;
	float thin;
	uint32_t ml;

	e->torque_pct = 0.0f;
	switch (e->mode) {
	case BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE:
		target_rpm = RATED_RPM;
		/* The load closes at rated speed. */
		e->torque_pct = e->rpm >= RATED_RPM ? (e->hot ? DERATE_TORQUE_PCT : RUN_TORQUE_PCT)
						    : IDLE_TORQUE_PCT;
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

	/* Thermal. */
	load = (e->torque_pct / 100.0f) * (e->rpm / RATED_RPM);
	target_c = e->rpm > 0.0f ? THERMOSTAT_C + LOAD_GAIN_C * load : AMBIENT_C;
	e->coolant_c += (target_c - e->coolant_c) * (dt_s / COOLANT_TAU_S);
	target_c = e->rpm > 0.0f ? e->coolant_c + OIL_RISE_C : AMBIENT_C;
	e->oil_c += (target_c - e->oil_c) * (dt_s / OIL_TAU_S);
	thin = CLAMP((e->oil_c - OIL_THIN_C) / OIL_THIN_SPAN_C, 0.0f, 1.0f);
	e->oil_kpa = e->rpm > 0.0f ? (OIL_KPA_IDLE + OIL_KPA_PER_RATED * e->rpm / RATED_RPM) *
					     (1.0f - OIL_THIN_LOSS * thin)
				   : 0.0f;

	if (!e->hot && e->coolant_c > HOT_ON_C) {
		e->hot = true;
		dtc_on(&dtcs[DTC_COOLANT], "coolant hot, derating");
	} else if (e->hot && e->coolant_c < HOT_OFF_C) {
		e->hot = false;
		dtc_off(&dtcs[DTC_COOLANT], "derate");
	}
	/* Judged from idle speed up: pressure builds as the engine starts. */
	if (e->rpm >= IDLE_RPM && e->oil_kpa < OIL_LOW_KPA) {
		dtc_on(&dtcs[DTC_OIL], "oil pressure low");
	} else if (e->rpm == 0.0f || e->oil_kpa > OIL_OK_KPA) {
		dtc_off(&dtcs[DTC_OIL], "oil pressure");
	}

	/* Electrical: P = T * w, S = P / pf, I = P / (3 V pf). */
	e->hz = e->rpm / 30.0f;
	e->v_ln = V_LN_RATED * e->rpm / RATED_RPM;
	e->real_w = RATED_W * (e->torque_pct / 100.0f) * (e->rpm / RATED_RPM);
	e->apparent_va = e->real_w / POWER_FACTOR;
	e->reactive_var = sqrtf(e->apparent_va * e->apparent_va - e->real_w * e->real_w);
	e->i_a = e->v_ln > 0.0f ? e->real_w / (3.0f * e->v_ln * POWER_FACTOR) : 0.0f;
	e->fuel_lph = e->rpm > 0.0f ? IDLE_LPH + LPKWH * e->real_w / 1000.0f : 0.0f;

	/* Integrals. */
	if (e->rpm > 0.0f) {
		e->run_ms += LOOP_MS;
	}
	e->rev_frac += e->rpm * dt_s / 60.0f;
	e->revolutions += carry(&e->rev_frac);
	e->wh_frac += e->real_w * dt_s / 3600.0f;
	e->wh += carry(&e->wh_frac);
	e->ml_frac += e->fuel_lph * dt_s / 3.6f;
	ml = carry(&e->ml_frac);
	e->fuel_ml += ml;
	e->trip_ml += ml;
}

/* A packed PG, its unused bits set to not available, into the node's message. */
static void put(enum msg m, uint8_t *d)
{
	for (int i = 0; i < 8; i++) {
		d[i] |= na[m][i];
	}
	zelos_j1939_set(&msgs[m], d, 8);
}

#define ENC(name, signal, value) genset_##name##_##signal##_encode(value)
#define PUT(m, name, ...)                                                                          \
	do {                                                                                       \
		const struct genset_##name##_t s = {__VA_ARGS__};                                  \
		(void)genset_##name##_pack(d, &s, 8);                                              \
		put(m, d);                                                                         \
	} while (0)

/* 32-bit raw values in integer arithmetic: the codec's float encode would lose their low bits. */
#define POWER_RAW(w) ((uint32_t)((int64_t)(w) + 2000000000LL))

/*
 * DM1 (active) or DM2 (previously active): the lamps the listed DTCs light,
 * then the DTCs, J1939-73. None leaves bytes 3-6 zero; one fits the frame,
 * bytes 7-8 not available; two go by transport, 10 bytes. The flash byte is
 * not available.
 */
static uint16_t encode_dm(uint8_t *d, bool active)
{
	struct genset_dm1_t m = {0};
	uint16_t len = 2;

	BUILD_ASSERT(ARRAY_SIZE(dtcs) == 2, "two DTC slots in the codec");
	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		const struct dtc *dtc = &dtcs[i];

		if (dtc->active != active || dtc->oc == 0U) {
			continue;
		}
		if (dtc->lamp == LAMP_AMBER) {
			m.amber_warning_lamp = 1;
		} else {
			m.red_stop_lamp = 1;
		}
		if (len == 2) {
			m.dtc1_spn_low = dtc->spn & 0xFFFFU;
			m.dtc1_spn_high = dtc->spn >> 16;
			m.dtc1_fmi = dtc->fmi;
			m.dtc1_oc = dtc->oc;
		} else {
			m.dtc2_spn_low = dtc->spn & 0xFFFFU;
			m.dtc2_spn_high = dtc->spn >> 16;
			m.dtc2_fmi = dtc->fmi;
			m.dtc2_oc = dtc->oc;
		}
		len += 4;
	}
	/* DM2 shares DM1's layout. */
	(void)genset_dm1_pack(d, &m, GENSET_DM1_LENGTH);
	d[1] = 0xFF;
	if (len <= 8) {
		d[6] = d[7] = 0xFF;
		len = 8;
	}
	return len;
}

static void publish(const struct engine *e)
{
	uint8_t d[ZELOS_J1939_MAX_LEN];
	const bool running = e->rpm > 0.0f;
	const float load = (e->torque_pct / 100.0f) * (e->rpm / RATED_RPM);
	const float v_ll = e->v_ln * SQRT3;

	PUT(M_EEC1, eec1, .actual_engine_percent_torque = ENC(eec1, actual_engine_percent_torque, e->torque_pct),
	    .engine_speed = ENC(eec1, engine_speed, e->rpm));
	PUT(M_EEC2, eec2, .accelerator_pedal_position1 = ENC(eec2, accelerator_pedal_position1, e->torque_pct),
	    .engine_percent_load_at_current_speed = ENC(eec2, engine_percent_load_at_current_speed, e->torque_pct));
	PUT(M_ET1, et1, .engine_coolant_temp = ENC(et1, engine_coolant_temp, e->coolant_c),
	    .engine_fuel_temp1 = ENC(et1, engine_fuel_temp1, AMBIENT_C + 0.35f * (e->coolant_c - AMBIENT_C)),
	    .engine_oil_temp1 = ENC(et1, engine_oil_temp1, e->oil_c));
	PUT(M_EFLP1, efl_p1, .engine_fuel_delivery_pressure = ENC(efl_p1, engine_fuel_delivery_pressure, running ? FUEL_KPA : 0.0f),
	    .engine_oil_level = ENC(efl_p1, engine_oil_level, OIL_LEVEL_PCT),
	    .engine_oil_pressure = ENC(efl_p1, engine_oil_pressure, e->oil_kpa),
	    .engine_coolant_pressure = ENC(efl_p1, engine_coolant_pressure, running ? 80.0f + 0.6f * (e->coolant_c - AMBIENT_C) : 0.0f),
	    .engine_coolant_level = ENC(efl_p1, engine_coolant_level, COOLANT_LVL_PCT));
	PUT(M_VEP1, vep1, .net_battery_current = ENC(vep1, net_battery_current, running ? NET_CHARGE_A : NET_DISCHARGE_A),
	    .alternator_current = ENC(vep1, alternator_current, running ? ALTERNATOR_A : 0.0f),
	    .alternator_potential = ENC(vep1, alternator_potential, running ? ALTERNATOR_V : 0.0f),
	    .electrical_potential = ENC(vep1, electrical_potential, running ? CHARGING_V : BATTERY_V));
	PUT(M_HOURS, hours, .engine_total_hours_of_operation = (uint32_t)(e->run_ms / 180000U), /* 0.05 h */
	    .engine_total_revolutions = (uint32_t)(e->revolutions / 1000U));
	PUT(M_LFC, lfc, .engine_trip_fuel = (uint32_t)(e->trip_ml / 500U), /* 0.5 L */
	    .engine_total_fuel_used = (uint32_t)(e->fuel_ml / 500U));
	PUT(M_LFE, lfe, .engine_fuel_rate = ENC(lfe, engine_fuel_rate, e->fuel_lph),
	    .engine_instantaneous_fuel_economy = 0xFFFF, /* not available: a genset goes nowhere */
	    .engine_throttle_valve1_position1 = ENC(lfe, engine_throttle_valve1_position1, e->torque_pct));
	PUT(M_AMB, amb, .barometric_pressure = ENC(amb, barometric_pressure, BARO_KPA),
	    .ambient_air_temp = ENC(amb, ambient_air_temp, AMBIENT_C),
	    .engine_air_inlet_temp = ENC(amb, engine_air_inlet_temp, AMBIENT_C + 10.0f * load));
	PUT(M_IC1, ic1, .engine_intake_manifold1_pressure = ENC(ic1, engine_intake_manifold1_pressure, running ? 100.0f + 150.0f * load : 0.0f),
	    .engine_intake_manifold1_temp = ENC(ic1, engine_intake_manifold1_temp, AMBIENT_C + 15.0f + 30.0f * load));

	/* Balanced: every phase reads the average; a phase's line-line voltage is to the next phase. */
	PUT(M_GAAC, gaac, .generator_average_line_line_acrms_voltage = ENC(gaac, generator_average_line_line_acrms_voltage, v_ll),
	    .generator_average_line_neutral_acrms_voltage = ENC(gaac, generator_average_line_neutral_acrms_voltage, e->v_ln),
	    .generator_average_ac_frequency = ENC(gaac, generator_average_ac_frequency, e->hz),
	    .generator_average_acrms_current = ENC(gaac, generator_average_acrms_current, e->i_a));
	PUT(M_GPAAC, gpaac, .generator_phase_a_line_line_acrms_voltage = ENC(gpaac, generator_phase_a_line_line_acrms_voltage, v_ll),
	    .generator_phase_a_line_neutral_acrms_voltage = ENC(gpaac, generator_phase_a_line_neutral_acrms_voltage, e->v_ln),
	    .generator_phase_aac_frequency = ENC(gpaac, generator_phase_aac_frequency, e->hz),
	    .generator_phase_aacrms_current = ENC(gpaac, generator_phase_aacrms_current, e->i_a));
	PUT(M_GPBAC, gpbac, .generator_phase_b_line_line_acrms_voltage = ENC(gpbac, generator_phase_b_line_line_acrms_voltage, v_ll),
	    .generator_phase_b_line_neutral_acrms_voltage = ENC(gpbac, generator_phase_b_line_neutral_acrms_voltage, e->v_ln),
	    .generator_phase_bac_frequency = ENC(gpbac, generator_phase_bac_frequency, e->hz),
	    .generator_phase_bacrms_current = ENC(gpbac, generator_phase_bacrms_current, e->i_a));
	PUT(M_GPCAC, gpcac, .generator_phase_c_line_line_acrms_voltage = ENC(gpcac, generator_phase_c_line_line_acrms_voltage, v_ll),
	    .generator_phase_c_line_neutral_acrms_voltage = ENC(gpcac, generator_phase_c_line_neutral_acrms_voltage, e->v_ln),
	    .generator_phase_cac_frequency = ENC(gpcac, generator_phase_cac_frequency, e->hz),
	    .generator_phase_cacrms_current = ENC(gpcac, generator_phase_cacrms_current, e->i_a));
	PUT(M_GTACP, gtacp, .generator_total_real_power = POWER_RAW(e->real_w),
	    .generator_total_apparent_power = POWER_RAW(e->apparent_va));
	PUT(M_GTACR, gtacr, .generator_total_reactive_power = POWER_RAW(e->reactive_var),
	    .generator_overall_power_factor = ENC(gtacr, generator_overall_power_factor, POWER_FACTOR),
	    .generator_overall_power_factor_lagging = PF_LAGGING);
	PUT(M_GTACE, gtace, .generator_totalk_wh_export = (uint32_t)(e->wh / 1000U),
	    .generator_totalk_wh_import = 0);

	zelos_j1939_set(&msgs[M_DM1], d, encode_dm(d, true));
	zelos_j1939_set(&msgs[M_DM2], d, encode_dm(d, false));
}

static struct engine engine = {
	.mode = BENCH_VCU_COMMAND_REQUESTED_MODE_STANDBY_CHOICE,
	.coolant_c = AMBIENT_C,
	.oil_c = AMBIENT_C,
	.run_ms = HISTORY_H * 3600000ULL,
	.revolutions = HISTORY_H * 60ULL * 1800ULL,
	.wh = HISTORY_H * HISTORY_W,
	.fuel_ml = HISTORY_H * HISTORY_ML_PER_H,
};

static void run(void *p1, void *p2, void *p3)
{
	int64_t deadline = k_uptime_get();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	engine.last_command_ms = deadline;

	for (;;) {
		follow_vcu(&engine, k_uptime_get());
		step(&engine);
		publish(&engine);

		deadline += LOOP_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}

/*
 * The model, on a thread of its own so an image can run it beside other
 * nodes; main()'s priority, where it ran before, and room for the float
 * model and twenty packs.
 */
K_THREAD_DEFINE(genset_tid, 2048, run, NULL, NULL, NULL, 0, 0, SYS_FOREVER_MS);

int genset_start(void)
{
	if (zelos_can_subscribe(BENCH_VCU_COMMAND_FRAME_ID, &genset_vcu_msgq) < 0) {
		LOG_ERR("subscribe to VCU_Command failed");
		return -EIO;
	}

	publish(&engine);
	if (zelos_j1939_start(&j1939) != 0) {
		return -EIO;
	}

	LOG_INF("genset up, NAME 0x%016llx", (unsigned long long)NAME);
	k_thread_start(genset_tid);

	return 0;
}
