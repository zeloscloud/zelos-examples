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
 * on request only, like SOFT and CI: by RTS/CTS to the requester, by BAM to
 * everyone. A controller's TSC1 overrides the governor: the engine follows the
 * requested speed or torque and names the controller in EEC1.
 *
 * A service tool gets the rest of J1939-73 on request: the other DTC lists
 * (DM6, DM12, DM23, DM27, DM28), readiness and counts that agree with them
 * (DM5, DM21, DM26, DM29), calibration, ratios, supported SPNs and a freeze
 * frame (DM19, DM20, DM24, DM25), lamps, timers, NTE status and engine family
 * (DM31, DM33, DM34, DM56). It clears DTCs (DM3, DM11, DM22), commands a test
 * (DM7, answered by DM30) and reads and writes a small memory behind a seed
 * and key (DM14, DM15, DM16). A cleared fault whose cause persists is back on
 * the next model step.
 */

#include <bench.h>
#include <genset.h>
#include <zelos/can.h>
#include <zelos/j1939.h>

#include "genset_node.h"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(genset, LOG_LEVEL_INF);

/* A self-configurable address, so the NAME says arbitrary-address capable. */
#define PREFERRED_ADDRESS 0x80U

/* Function 0 is Engine. Manufacturer 0 and the identity are examples, not a product. */
#define NAME ZELOS_J1939_NAME(1, 0, 0, 0, 0, 0, 0, 0, 0x0A11)

/* EEC2 is the fastest PG, every 50 ms; each send carries a fresh model step. */
#define LOOP_MS 50U

/* The VCU sends every 100 ms; after this long without it, fall back to standby. */
#define VCU_TIMEOUT_MS 500

/* TSC1 comes every 10 ms while a controller overrides; without it this long, the governor is back. */
#define TSC1_TIMEOUT_MS 1000
#define PGN_TSC1        0U
/* PF byte and the two data-page bits: every frame of PGN 0, any destination. */
#define PF_MASK 0x03FF0000U

/* TSC1 override control modes, SPN 695. */
#define TSC1_OFF    0U
#define TSC1_SPEED  1U
#define TSC1_TORQUE 2U
#define TSC1_LIMIT  3U

/* EEC1 engine torque mode, SPN 899, and starter mode, SPN 1675. */
#define TORQUE_MODE_LOW_IDLE  0U
#define TORQUE_MODE_PEDAL     1U
#define TORQUE_MODE_LIMITING  8U
#define TORQUE_MODE_REMOTE    11U
#define STARTER_NOT_REQUESTED 0U
#define STARTER_ENGAGED       2U
#define STARTER_INHIBITED_RUN 4U

/* CCVS1 PTO governor state, SPN 976. */
#define PTO_OFF     0U
#define PTO_STANDBY 3U
#define PTO_SET     5U

/* EEC3 friction and parasitic torque: a few percent, rising with speed. */
#define FRICTION_PCT_IDLE     10.0f
#define FRICTION_PCT_PER_RATED 5.0f
#define PARASITIC_PCT         3.0f
#define PUMPING_PCT           (-3.0f)

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

/* J1939-73 PGNs the DBC does not describe: variable length, or a request with no data. */
#define PGN_DM3  65228U
#define PGN_DM4  65229U
#define PGN_DM6  65231U
#define PGN_DM11 65235U
#define PGN_DM12 65236U
#define PGN_DM16 55040U
#define PGN_DM19 54016U
#define PGN_DM20 49664U
#define PGN_DM23 64949U
#define PGN_DM24 64950U
#define PGN_DM25 64951U
#define PGN_DM27 64898U
#define PGN_DM28 64896U
#define PGN_DM30 41984U
#define PGN_DM31 41728U
#define PGN_DM33 41216U
#define PGN_DM56 64711U
#define PGN_DM35 40704U
#define PGN_DM42 64862U
#define PGN_DM58 64475U

/* DM5 and DM26. Compliance 17: engine manufacturer diagnostics. */
#define OBD_COMPLIANCE 17U
/* Fuel system and comprehensive component monitors, both complete. */
#define MONITORS_CONTINUOUS 0x06U
/* Exhaust gas sensor (byte 5 bit 6) and boost pressure control (byte 6 bit 2); the second not complete. */
#define MONITORS_SUPPORT    0x0220U
#define MONITORS_INCOMPLETE 0x0200U
/* A warm-up: the coolant from below the first to above the second. */
#define WARM_OFF_C 40.0f
#define WARM_ON_C  70.0f
/* 16-bit times and counts stop below the not-available and error ranges. */
#define MAX_16 64255U
#define MAX_8  250U

/* DM7 and DM30: the test identifier for every test of an SPN, and the special values. */
#define TEST_ALL         247U
#define TEST_RATIONALITY 245U
#define SPN_ENGINE_SPEED 190U
#define TEST_CANNOT_RUN  0xFB01U
#define TEST_NO_LIMIT    0xFFFFU
#define SLOT_UNAVAILABLE 0xFFFFU

/* DM22 control bytes; a positive answer is the command + 1, a negative one + 2. */
#define DM22_CLEAR_PREVIOUS      1U
#define DM22_CLEAR_ACTIVE        17U
#define DM22_UNKNOWN_DTC         2U
#define DM22_NOT_PREVIOUS_ACTIVE 3U
#define DM22_NOT_ACTIVE          4U

/*
 * DM14 to DM16: 64 bytes at pointer 0, byte n starting as 0xA0 + n. A request
 * is first answered with a seed; repeated with the key, seed ^ MEM_KEY, it
 * proceeds. A read is as long as one transport message carries after DM16's
 * count byte; a write is one frame, as the node receives no transport.
 */
#define MEM_SIZE      64U
#define MEM_KEY       0x1939U
#define MEM_READ_MAX  (ZELOS_J1939_MAX_LEN - 1U)
#define MEM_WRITE_MAX 7U
#define MEM_READ      1U /* DM14 commands */
#define MEM_WRITE     2U
#define MEM_COMPLETED 4U
#define MEM_FAILED    5U
#define MEM_PROCEED   0U /* DM15 status, beside completed and failed */
#define MEM_NO_KEY    0xFFFFU
#define MEM_NO_ERROR  0xFFFFFFU
/* DM15 error indicators. */
#define MEM_ERR_UNKNOWN 0x1U
#define MEM_ERR_ADDRESS 0x100U
#define MEM_ERR_LENGTH  0x102U
#define MEM_ERR_KEY     0x1003U
/* DM15 EDCP extension: the EDC parameter is an error indicator. */
#define MEM_EDCP_ERROR 0x07U

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
	/* Past its early threshold: on the way to active, or active. */
	bool pending;
	/* Emission-related: the derate. */
	bool emission;
};

/* Coolant temperature and oil pressure, each FMI "most severe". */
static struct dtc dtcs[] = {
	{.spn = 110, .fmi = 0, .lamp = LAMP_AMBER, .oc = 1, .emission = true},
	{.spn = 100, .fmi = 1, .lamp = LAMP_RED, .oc = 1},
};
#define DTC_COOLANT 0
#define DTC_OIL     1

/* The DTC lists, one per DM. */
enum list {
	LIST_ACTIVE,           /* DM1 */
	LIST_PREVIOUS,         /* DM2 */
	LIST_PENDING_EMISSION, /* DM6 */
	LIST_MIL_ON,           /* DM12: active and emission-related */
	LIST_PREVIOUS_MIL_ON,  /* DM23 */
	LIST_PENDING,          /* DM27 */
	LIST_PERMANENT,        /* DM28: both, whatever is cleared */
};

static bool listed(const struct dtc *dtc, enum list list)
{
	const bool previous = !dtc->active && dtc->oc > 0U;

	switch (list) {
	case LIST_ACTIVE:
		return dtc->active;
	case LIST_PREVIOUS:
		return previous;
	case LIST_PENDING_EMISSION:
		return dtc->pending && dtc->emission;
	case LIST_MIL_ON:
		return dtc->active && dtc->emission;
	case LIST_PREVIOUS_MIL_ON:
		return previous && dtc->emission;
	case LIST_PENDING:
		return dtc->pending;
	default:
		return true;
	}
}

static uint8_t count(enum list list)
{
	uint8_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		n += listed(&dtcs[i], list) ? 1U : 0U;
	}
	return n;
}

/* J1939-73 DTC, 4 bytes: SPN, FMI beside the SPN's top bits, occurrence count. */
static void put_dtc(uint8_t *d, const struct dtc *dtc)
{
	sys_put_le16(dtc->spn & 0xFFFFU, d);
	d[2] = (uint8_t)(((dtc->spn >> 16) << 5) | dtc->fmi);
	d[3] = dtc->oc;
}

/* A request or command for the diagnostics, from the J1939 thread to the model's. */
struct diag_rx {
	uint32_t pgn;
	uint8_t sa;
	/* Addressed to this node, not to everyone. */
	bool to_us;
	uint8_t len;
	uint8_t data[8];
};

K_MSGQ_DEFINE(genset_vcu_msgq, sizeof(struct can_frame), 4, 4);
K_MSGQ_DEFINE(genset_tsc1_msgq, sizeof(struct can_frame), 8, 4);
K_MSGQ_DEFINE(genset_diag_msgq, sizeof(struct diag_rx), 8, 4);

/*
 * The node's messages. The codec packs the first N_PG, 8 bytes each; DM1 and
 * the other DTC lists grow with their DTCs; SOFT is text. The four at the end
 * answer a command, not a request.
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
	M_EEC3,
	M_CCVS,
	M_DM5,
	M_DM21,
	M_DM26,
	M_DM29,
	M_DM34,
	N_PG,
	M_DM1 = N_PG,
	M_DM2,
	M_SOFT,
	M_CI,
	M_DM6,
	M_DM12,
	M_DM23,
	M_DM27,
	M_DM28,
	M_DM19,
	M_DM20,
	M_DM24,
	M_DM25,
	M_DM31,
	M_DM33,
	M_DM56,
	M_DM15,
	M_DM16,
	M_DM22,
	M_DM30,
	M_DM4,
	M_DM35,
	M_DM42,
	M_DM58,
	N_MSGS,
};

#define DTC_LIST_LEN (2U + 4U * ARRAY_SIZE(dtcs))

static uint8_t pg[N_PG][8];
static uint8_t dm1[ZELOS_J1939_MAX_LEN];
static uint8_t dm2[ZELOS_J1939_MAX_LEN];
static uint8_t dm6[DTC_LIST_LEN];
/* DM35 and DM42 list the active DTCs, as DM1. */
static uint8_t dm35[DTC_LIST_LEN];
static uint8_t dm42[DTC_LIST_LEN];
static uint8_t dm12[DTC_LIST_LEN];
static uint8_t dm23[DTC_LIST_LEN];
static uint8_t dm27[DTC_LIST_LEN];
static uint8_t dm28[DTC_LIST_LEN];
/* DM19: the calibration verification number, then the calibration's name in 16 bytes. */
static uint8_t dm19[20] = {0x1B, 0xCA, 0x11, 0x0A, 'Z', 'E', 'L', 'O', 'S', '-', 'G', 'E', 'N', 'S', 'E', 'T'};
/*
 * DM20: 120 ignition cycles, 100 with monitoring conditions met, then per
 * monitor its SPN (the 5 bits above it ones), numerator and denominator:
 * coolant 95/100, oil pressure 98/100, engine speed not supported.
 */
static uint8_t dm20[] = {
	120, 0, 100, 0,
	110, 0, 0xF8, 95, 0, 100, 0,
	100, 0, 0xF8, 98, 0, 100, 0,
	190, 0, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF,
};
/*
 * DM24, per SPN: byte 3 bit 1 clear when in the freeze frame, bit 2 in the
 * data stream, bit 3 in test results, bits 4-5 ones; byte 4 its length. The
 * freeze frame holds the first four, in this order.
 */
#define DM24_DATA_STREAM 0x1DU
#define DM24_FREEZE      0x1CU /* and the data stream */
#define DM24_TESTED      0x18U /* and both */
static uint8_t dm24[] = {
	190, 0, DM24_FREEZE, 2,      /* engine speed */
	110, 0, DM24_TESTED, 1,      /* coolant temperature */
	100, 0, DM24_TESTED, 1,      /* oil pressure */
	92,  0, DM24_FREEZE, 1,      /* percent load */
	175, 0, DM24_DATA_STREAM, 2, /* oil temperature */
	183, 0, DM24_DATA_STREAM, 2, /* fuel rate */
};
#define FREEZE_DATA_LEN 5U
/* DM25: the bytes that follow, the DTC, its data; or the form of no freeze frame. */
static uint8_t dm25[1U + 4U + FREEZE_DATA_LEN] = {0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};
/* DM25 and DM31 with nothing to list. */
static const uint8_t no_record[8] = {0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};
/* DM31: per DTC its 4 bytes, then the lamp it lights and the flash byte. */
static uint8_t dm31[6U * ARRAY_SIZE(dtcs)];
/* DM33: timer number 1, minutes derated, and a second timer, not available. */
static uint8_t dm33[9];
/* DM56: model year as 2026E-MY, then the engine family, ending in '*'. */
static uint8_t dm56[] = "2026E-MYZELOS GENSET*";
#define DM56_LEN (sizeof(dm56) - 1U)
static uint8_t dm15[8];
static uint8_t dm16[1U + MEM_READ_MAX];
static uint8_t dm22[8];
static uint8_t dm30[12];
/*
 * DM4: the bytes that follow, the DTC, then 8 data bytes of the node's own:
 * speed, coolant, oil pressure and load as their PGs carry them, 0xFF after.
 * From boot the frame of the coolant fault in the node's history.
 */
#define DM4_DATA_LEN 8U
static uint8_t dm4[1U + 4U + DM4_DATA_LEN];
/* DM58: the test identifier, the SPN, its data as its PG carries it. */
static uint8_t dm58[8];
/* J1939-71 SOFT: the number of fields, then each field ending in '*'. */
static uint8_t soft[] = "\x03" "ZELOS-GENSET*" "v1.1.0*" "zelos-examples node genset*";
#define SOFT_LEN (sizeof(soft) - 1U)
BUILD_ASSERT(SOFT_LEN > 8U && SOFT_LEN <= ZELOS_J1939_MAX_LEN, "SOFT goes by transport");
/* J1939-71 CI: make, model, serial number and unit number, each ending in '*'. */
static uint8_t ci[] = "Zelos*GENSET-FULL*0A11*ZELOSBENCH*";
#define CI_LEN (sizeof(ci) - 1U)
BUILD_ASSERT(CI_LEN == GENSET_CI_LENGTH && CI_LEN > 8U, "CI goes by transport, as long as the DBC says");

/* dbc/genset.dbc is the one list: PGN and priority from the frame id, period from the cycle time. */
#define PGN_OF(id)  (((id) >> 8) & ((((id) >> 16) & 0xFFU) < 240U ? 0x3FF00U : 0x3FFFFU))
#define PRIO_OF(id) (((id) >> 26) & 7U)
#define PG(m, name, offset)                                                                    \
	[m] = {.pgn = PGN_OF(GENSET_##name##_FRAME_ID), .priority = PRIO_OF(GENSET_##name##_FRAME_ID), \
	       .period_ms = GENSET_##name##_CYCLE_TIME_MS, .offset_ms = offset, .len = 8,          \
	       .data = pg[m]}
/* On request only: 8 bytes the codec packs, or longer and packed here. */
#define DM8(m, name)                                                                           \
	[m] = {.pgn = PGN_OF(GENSET_##name##_FRAME_ID), .priority = PRIO_OF(GENSET_##name##_FRAME_ID), \
	       .len = 8, .data = pg[m]}
#define DM(m, n, buf, length) [m] = {.pgn = n, .priority = 6, .len = length, .data = buf}
/* Sent to whoever commanded it. */
#define REPLY(m, n, prio, buf)                                                                 \
	[m] = {.pgn = n, .priority = prio, .len = sizeof(buf), .data = buf, .reply_only = true}

/*
 * Offsets spread the periodic sends over the 10 ms ticks: the nine 100 ms
 * PGs one per tick, EEC2 (50 ms) at 30 and 80 ms of each 100, EEC3 (250 ms)
 * at 40 and 90 in turn, the slower ones at 90 in turn, so no tick carries
 * more than two periodic frames. The bus sees about 125 frames a second.
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
	PG(M_CCVS, CCVS1, 80),
	PG(M_EEC3, EEC3, 140),
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
	[M_CI] = {.pgn = PGN_OF(GENSET_CI_FRAME_ID), .priority = PRIO_OF(GENSET_CI_FRAME_ID),
		  .period_ms = 0, .len = CI_LEN, .data = ci},
	DM8(M_DM5, DM5),
	DM8(M_DM21, DM21),
	DM8(M_DM26, DM26),
	DM8(M_DM29, DM29),
	DM8(M_DM34, DM34),
	DM(M_DM6, PGN_DM6, dm6, 8),
	DM(M_DM12, PGN_DM12, dm12, 8),
	DM(M_DM23, PGN_DM23, dm23, 8),
	DM(M_DM27, PGN_DM27, dm27, 8),
	DM(M_DM28, PGN_DM28, dm28, 8),
	DM(M_DM19, PGN_DM19, dm19, sizeof(dm19)),
	DM(M_DM20, PGN_DM20, dm20, sizeof(dm20)),
	DM(M_DM24, PGN_DM24, dm24, sizeof(dm24)),
	DM(M_DM25, PGN_DM25, dm25, 8),
	DM(M_DM31, PGN_DM31, dm31, 8),
	DM(M_DM33, PGN_DM33, dm33, sizeof(dm33)),
	DM(M_DM56, PGN_DM56, dm56, DM56_LEN),
	/* DM15 before DM16: a poll that sends both sends them in this order. */
	REPLY(M_DM15, PGN_OF(GENSET_DM15_FRAME_ID), 6, dm15),
	REPLY(M_DM16, PGN_DM16, 7, dm16),
	REPLY(M_DM22, PGN_OF(GENSET_DM22_FRAME_ID), 6, dm22),
	REPLY(M_DM30, PGN_DM30, 6, dm30),
	DM(M_DM4, PGN_DM4, dm4, sizeof(dm4)),
	DM(M_DM35, PGN_DM35, dm35, 8),
	DM(M_DM42, PGN_DM42, dm42, 8),
	REPLY(M_DM58, PGN_DM58, 6, dm58),
};

/* Which list each DTC-list DM carries. */
static const struct {
	enum msg msg;
	enum list list;
} dtc_lists[] = {
	{M_DM1, LIST_ACTIVE},      {M_DM2, LIST_PREVIOUS},         {M_DM6, LIST_PENDING_EMISSION},
	{M_DM12, LIST_MIL_ON},     {M_DM23, LIST_PREVIOUS_MIL_ON}, {M_DM27, LIST_PENDING},
	{M_DM28, LIST_PERMANENT},  {M_DM35, LIST_ACTIVE},          {M_DM42, LIST_ACTIVE},
};

/* The bits of each PG the DBC leaves out, sent as ones: not available. */
static const uint8_t na[N_PG][8] = {
	[M_EEC2] = {0xC0, 0, 0, 0xFF, 0xFF, 0, 0, 0},
	[M_EEC3] = {0, 0, 0, 0xFF, 0, 0xFF, 0xFF, 0xFF},
	[M_CCVS] = {0xF3, 0xFF, 0xFF, 0xC0, 0, 0xFF, 0, 0},
	[M_ET1] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF},
	[M_EFLP1] = {0, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0},
	[M_VEP1] = {0, 0, 0, 0, 0, 0, 0xFF, 0xFF},
	[M_LFE] = {0, 0, 0, 0, 0xFF, 0xFF, 0, 0xFF},
	[M_AMB] = {0, 0xFF, 0xFF, 0, 0, 0, 0xFF, 0xFF},
	[M_IC1] = {0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	[M_GTACR] = {0, 0, 0, 0, 0, 0, 0xFC, 0xFF},
	[M_DM29] = {0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF},
	[M_DM34] = {0x03, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
};

/* The J1939 thread hands requests and commands to the model's thread. */
static bool on_request(uint32_t pgn, uint8_t sa, uint8_t da, void *user)
{
	const struct diag_rx rx = {.pgn = pgn, .sa = sa, .to_us = da != ZELOS_J1939_ADDR_GLOBAL};

	ARG_UNUSED(user);

	if (pgn != PGN_DM3 && pgn != PGN_DM11) {
		return false;
	}
	/* A full queue leaves it untaken: one sent to the node is NACKed. */
	return k_msgq_put(&genset_diag_msgq, &rx, K_NO_WAIT) == 0;
}

static void on_pg(uint32_t pgn, uint8_t sa, const uint8_t *data, uint8_t len, void *user)
{
	struct diag_rx rx = {.pgn = pgn, .sa = sa, .to_us = true, .len = len};

	ARG_UNUSED(user);

	memcpy(rx.data, data, len);
	(void)k_msgq_put(&genset_diag_msgq, &rx, K_NO_WAIT);
}

#define PGN_DM7  PGN_OF(GENSET_DM7_FRAME_ID)
#define PGN_DM14 PGN_OF(GENSET_DM14_FRAME_ID)
#define PGN_DM22 PGN_OF(GENSET_DM22_FRAME_ID)

static const uint32_t rx_pgns[] = {PGN_DM7, PGN_DM14, PGN_DM16, PGN_DM22};

static struct zelos_j1939 j1939 = {
	.name = NAME,
	.preferred = PREFERRED_ADDRESS,
	.msgs = msgs,
	.n_msgs = ARRAY_SIZE(msgs),
	.on_request = on_request,
	.on_pg = on_pg,
	.rx_pgns = rx_pgns,
	.n_rx_pgns = ARRAY_SIZE(rx_pgns),
};

/* What a TSC1 sender asks of the engine, while its messages keep coming. */
struct override {
	uint8_t mode;
	uint8_t sa;
	int64_t last_ms;
	float rpm;
	/* Requested torque, or a limit; negative when the sender left it not available. */
	float torque_pct;
};

struct engine {
	uint8_t mode;
	int64_t last_command_ms;
	struct override tsc1;
	/* The governor's speed target and the mode's torque demand, before any override. */
	float target_rpm;
	float demand_pct;
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
	/* Diagnostics: this run, time derated, and since a tool last cleared DTCs. */
	uint32_t since_start_ms;
	uint32_t hot_ms;
	uint32_t since_clear_ms;
	uint8_t warmups;
	bool warm;
	/* A freeze frame is held, taken when the first DTC set. */
	bool frozen;
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

/* TSC1 addressed to this node: the last sender's request stands until it stops or times out. */
static void follow_tsc1(struct engine *e, int64_t now_ms)
{
	struct can_frame frame;
	struct genset_tsc1_t cmd;

	while (k_msgq_get(&genset_tsc1_msgq, &frame, K_NO_WAIT) == 0) {
		if (((frame.id >> 8) & 0xFFU) != j1939.address ||
		    genset_tsc1_unpack(&cmd, frame.data, can_dlc_to_bytes(frame.dlc)) != 0) {
			continue;
		}
		e->tsc1.mode = cmd.engine_override_control_mode;
		e->tsc1.sa = frame.id & 0xFFU;
		e->tsc1.last_ms = now_ms;
		e->tsc1.rpm = genset_tsc1_engine_requested_speed_speed_limit_decode(cmd.engine_requested_speed_speed_limit);
		e->tsc1.torque_pct = cmd.engine_requested_torque_torque_limit == 0xFFU
					     ? -1.0f
					     : genset_tsc1_engine_requested_torque_torque_limit_decode(cmd.engine_requested_torque_torque_limit);
	}

	if (e->tsc1.mode != TSC1_OFF && now_ms - e->tsc1.last_ms > TSC1_TIMEOUT_MS) {
		e->tsc1.mode = TSC1_OFF;
		LOG_INF("TSC1 from 0x%02x timed out", e->tsc1.sa);
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

	e->demand_pct = e->torque_pct;

	/* A controller's override, while the engine turns: it cannot start or stop it. */
	if (target_rpm > 0.0f) {
		const float rpm = CLAMP(e->tsc1.rpm, IDLE_RPM, RATED_RPM);
		const float torque = e->tsc1.torque_pct;

		switch (e->tsc1.mode) {
		case TSC1_SPEED:
			target_rpm = rpm;
			break;
		case TSC1_TORQUE:
			if (torque >= 0.0f) {
				e->torque_pct = MIN(torque, RUN_TORQUE_PCT);
			}
			break;
		case TSC1_LIMIT:
			target_rpm = MIN(target_rpm, rpm);
			if (torque >= 0.0f) {
				e->torque_pct = MIN(e->torque_pct, torque);
			}
			break;
		default:
			break;
		}
	}
	e->target_rpm = target_rpm;

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
	} else if (e->hot && e->coolant_c < HOT_OFF_C) {
		e->hot = false;
	}
	/* By level, not edge: cleared by a tool while still hot, it sets again. */
	if (e->hot) {
		dtc_on(&dtcs[DTC_COOLANT], "coolant hot, derating");
	} else {
		dtc_off(&dtcs[DTC_COOLANT], "derate");
	}
	/* Judged from idle speed up: pressure builds as the engine starts. */
	if (e->rpm >= IDLE_RPM && e->oil_kpa < OIL_LOW_KPA) {
		dtc_on(&dtcs[DTC_OIL], "oil pressure low");
	} else if (e->rpm == 0.0f || e->oil_kpa > OIL_OK_KPA) {
		dtc_off(&dtcs[DTC_OIL], "oil pressure");
	}
	/* Pending from the threshold each fault clears at, so before it is active. */
	dtcs[DTC_COOLANT].pending = e->hot || e->coolant_c >= HOT_OFF_C;
	dtcs[DTC_OIL].pending = dtcs[DTC_OIL].active ||
				(e->rpm >= IDLE_RPM && e->oil_kpa < OIL_OK_KPA);

	if (!e->warm && e->coolant_c > WARM_ON_C) {
		e->warm = true;
		e->warmups = (uint8_t)MIN(e->warmups + 1U, MAX_8);
	} else if (e->warm && e->coolant_c < WARM_OFF_C) {
		e->warm = false;
	}
	/* The timers stop at their top, 49 days on, rather than wrap. */
	e->since_start_ms = e->rpm > 0.0f ? MIN(e->since_start_ms, UINT32_MAX - LOOP_MS) + LOOP_MS : 0U;
	e->since_clear_ms = MIN(e->since_clear_ms, UINT32_MAX - LOOP_MS) + LOOP_MS;
	if (e->hot) {
		e->hot_ms = MIN(e->hot_ms, UINT32_MAX - LOOP_MS) + LOOP_MS;
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
 * DM1 (active), DM2 (previously active) or another DTC list: the lamps the
 * listed DTCs light, then the DTCs, J1939-73. None leaves bytes 3-6 zero; one
 * fits the frame, bytes 7-8 not available; two go by transport, 10 bytes. The
 * flash byte is not available.
 */
static uint16_t encode_dm(uint8_t *d, enum list list)
{
	struct genset_dm1_t m = {0};
	uint16_t len = 2;

	BUILD_ASSERT(ARRAY_SIZE(dtcs) == 2, "two DTC slots in the codec");
	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		const struct dtc *dtc = &dtcs[i];

		if (!listed(dtc, list)) {
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
	/* Every list shares DM1's layout. */
	(void)genset_dm1_pack(d, &m, GENSET_DM1_LENGTH);
	d[1] = 0xFF;
	if (len <= 8) {
		d[6] = d[7] = 0xFF;
		len = 8;
	}
	return len;
}

/* Who governs the engine, SPN 899, and who that is, SPN 1483. */
static uint8_t torque_mode(const struct engine *e)
{
	switch (e->tsc1.mode) {
	case TSC1_SPEED:
		return TORQUE_MODE_REMOTE;
	case TSC1_TORQUE:
	case TSC1_LIMIT:
		return TORQUE_MODE_LIMITING;
	default:
		return e->mode == BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE && e->rpm > 0.0f
			       ? TORQUE_MODE_PEDAL
			       : TORQUE_MODE_LOW_IDLE;
	}
}

static uint8_t starter_mode(const struct engine *e)
{
	if (e->rpm == 0.0f) {
		return STARTER_NOT_REQUESTED;
	}
	return e->rpm < IDLE_RPM && e->target_rpm > 0.0f ? STARTER_ENGAGED : STARTER_INHIBITED_RUN;
}

static uint8_t pto_state(const struct engine *e)
{
	if (e->rpm == 0.0f) {
		return PTO_OFF;
	}
	return e->mode == BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE ? PTO_SET : PTO_STANDBY;
}

static void publish(const struct engine *e)
{
	uint8_t d[ZELOS_J1939_MAX_LEN];
	uint16_t len;
	const bool running = e->rpm > 0.0f;
	const bool driving = e->mode == BENCH_VCU_COMMAND_REQUESTED_MODE_DRIVE_CHOICE;
	const float load = (e->torque_pct / 100.0f) * (e->rpm / RATED_RPM);
	const float v_ll = e->v_ln * SQRT3;

	PUT(M_EEC1, eec1, .engine_torque_mode = torque_mode(e),
	    .actual_engine_percent_torque_high_resolution = 0,
	    .drivers_demand_engine_percent_torque = ENC(eec1, drivers_demand_engine_percent_torque, e->demand_pct),
	    .actual_engine_percent_torque = ENC(eec1, actual_engine_percent_torque, e->torque_pct),
	    .engine_speed = ENC(eec1, engine_speed, e->rpm),
	    .source_address_of_controlling_device_for_engine_control = e->tsc1.mode != TSC1_OFF ? e->tsc1.sa : j1939.address,
	    .engine_starter_mode = starter_mode(e),
	    .engine_demand_percent_torque = ENC(eec1, engine_demand_percent_torque, e->torque_pct));
	PUT(M_EEC2, eec2, .accelerator_pedal1_low_idle_switch = running && e->demand_pct <= IDLE_TORQUE_PCT,
	    .accelerator_pedal_kickdown_switch = 0, .road_speed_limit_status = 0,
	    .accelerator_pedal_position1 = ENC(eec2, accelerator_pedal_position1, e->torque_pct),
	    .engine_percent_load_at_current_speed = ENC(eec2, engine_percent_load_at_current_speed, e->torque_pct),
	    .vehicle_acceleration_rate_limit_status = 0, .momentary_engine_maximum_power_enable_feedback = 0,
	    .dpf_thermal_management_active = 0, .scr_thermal_management_active = 0,
	    .actual_maximum_available_engine_percent_torque = ENC(eec2, actual_maximum_available_engine_percent_torque, e->hot ? DERATE_TORQUE_PCT : RUN_TORQUE_PCT),
	    .estimated_pumping_percent_torque = ENC(eec2, estimated_pumping_percent_torque, PUMPING_PCT));
	PUT(M_EEC3, eec3, .nominal_friction_percent_torque = ENC(eec3, nominal_friction_percent_torque, FRICTION_PCT_IDLE + FRICTION_PCT_PER_RATED * e->rpm / RATED_RPM),
	    .engines_desired_operating_speed = ENC(eec3, engines_desired_operating_speed, e->target_rpm),
	    .estimated_engine_parasitic_losses_percent_torque = ENC(eec3, estimated_engine_parasitic_losses_percent_torque, PARASITIC_PCT));
	PUT(M_CCVS, ccvs1, .parking_brake_switch = !driving, .cruise_control_active = 0, .cruise_control_enable_switch = 0,
	    .brake_switch = !running, .cruise_control_set_switch = 0, .cruise_control_coast_decelerate_switch = 0,
	    .cruise_control_resume_switch = 0, .cruise_control_accelerate_switch = 0,
	    .pto_governor_state = pto_state(e), .cruise_control_states = 0,
	    .engine_idle_increment_switch = 0, .engine_idle_decrement_switch = 0,
	    .engine_test_mode_switch = 0, .engine_shutdown_override_switch = 0);
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

	for (size_t i = 0; i < ARRAY_SIZE(dtc_lists); i++) {
		zelos_j1939_set(&msgs[dtc_lists[i].msg], d, encode_dm(d, dtc_lists[i].list));
	}

	/* Readiness and counts, of the lists as just sent. No MIL, and a genset goes nowhere: zeros. */
	PUT(M_DM5, dm5, .active_dtc_count = count(LIST_ACTIVE),
	    .previously_active_dtc_count = count(LIST_PREVIOUS),
	    .obd_compliance = OBD_COMPLIANCE,
	    .continuous_monitors = MONITORS_CONTINUOUS,
	    .non_continuous_supported = MONITORS_SUPPORT,
	    .non_continuous_not_complete = MONITORS_INCOMPLETE);
	PUT(M_DM21, dm21, .distance_mil_on = 0, .distance_since_clear = 0,
	    .time_mil_on = 0,
	    .time_since_clear = (uint16_t)MIN(e->since_clear_ms / 60000U, MAX_16));
	PUT(M_DM26, dm26, .time_since_engine_start = (uint16_t)MIN(e->since_start_ms / 1000U, MAX_16),
	    .warm_ups_since_clear = e->warmups,
	    .continuous_monitors_this_trip = MONITORS_CONTINUOUS,
	    .non_continuous_enabled = MONITORS_SUPPORT,
	    .non_continuous_not_complete_this_trip = MONITORS_INCOMPLETE);
	PUT(M_DM29, dm29, .pending_dtc_count = count(LIST_PENDING_EMISSION), .all_pending_dtc_count = count(LIST_PENDING),
	    .mil_on_dtc_count = count(LIST_MIL_ON), .previously_mil_on_dtc_count = count(LIST_PREVIOUS_MIL_ON),
	    .permanent_dtc_count = count(LIST_PERMANENT));
	/* In the control areas under load; no carve-out; deficiencies not available. */
	PUT(M_DM34, dm34, .nox_deficiency_area = 3, .nox_carve_out_area = 0,
	    .nox_control_area = load >= 0.5f, .pm_deficiency_area = 3,
	    .pm_carve_out_area = 0, .pm_control_area = load >= 0.5f);

	/* DM31: every DTC that is or was active, with its lamp on. */
	len = 0;
	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		if (dtcs[i].active || dtcs[i].oc > 0U) {
			put_dtc(&d[len], &dtcs[i]);
			d[len + 4U] = dtcs[i].lamp == LAMP_AMBER ? 0x04U : 0x10U;
			d[len + 5U] = 0xFF;
			len += 6U;
		}
	}
	if (len == 0U) {
		memcpy(d, no_record, sizeof(no_record));
		len = sizeof(no_record);
	}
	zelos_j1939_set(&msgs[M_DM31], d, len);

	d[0] = 1;
	sys_put_le32(e->hot_ms / 60000U, &d[1]);
	sys_put_le32(0xFFFFFFFFU, &d[5]);
	zelos_j1939_set(&msgs[M_DM33], d, sizeof(dm33));
}

/* DM4: the DTC and the 5 snapshot bytes of DM25, padded to DM4's 8. */
static void freeze_dm4(const uint8_t *dtc, const uint8_t *data)
{
	uint8_t d[sizeof(dm4)];

	memset(d, 0xFF, sizeof(d));
	d[0] = sizeof(d) - 1U;
	memcpy(&d[1], dtc, 4);
	memcpy(&d[5], data, FREEZE_DATA_LEN);
	zelos_j1939_set(&msgs[M_DM4], d, sizeof(d));
}

/*
 * DM25: when the first DTC sets, its 4 bytes and the freeze-frame SPNs of
 * DM24, in DM24's order and lengths, as their PGs carry them. Held while a DTC
 * is active.
 */
static void freeze(struct engine *e)
{
	uint8_t d[sizeof(dm25)];

	if (count(LIST_ACTIVE) == 0U) {
		if (e->frozen) {
			zelos_j1939_set(&msgs[M_DM25], no_record, sizeof(no_record));
			e->frozen = false;
		}
		return;
	}
	if (e->frozen) {
		return;
	}
	e->frozen = true;

	d[0] = sizeof(d) - 1U;
	put_dtc(&d[1], dtcs[DTC_COOLANT].active ? &dtcs[DTC_COOLANT] : &dtcs[DTC_OIL]);
	sys_put_le16(ENC(eec1, engine_speed, e->rpm), &d[5]);
	d[7] = ENC(et1, engine_coolant_temp, e->coolant_c);
	d[8] = ENC(efl_p1, engine_oil_pressure, e->oil_kpa);
	d[9] = ENC(eec2, engine_percent_load_at_current_speed, e->torque_pct);
	zelos_j1939_set(&msgs[M_DM25], d, sizeof(d));
	freeze_dm4(&d[1], &d[5]);
}

/* DM11 clears the active DTCs, DM3 the previously active: gone, their counts with them. */
static void clear_dtcs(struct engine *e, bool active)
{
	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		if (dtcs[i].active == active) {
			dtcs[i].active = false;
			dtcs[i].oc = 0;
		}
	}
	e->since_clear_ms = 0;
	e->warmups = 0;
}

/* DM22: clear one DTC, named by SPN and FMI; the answer is DM22 to the sender. */
static void clear_one(const struct diag_rx *rx)
{
	const uint8_t control = rx->data[0];
	const uint32_t spn = sys_get_le16(&rx->data[5]) | ((uint32_t)(rx->data[7] >> 5) << 16);
	const uint8_t fmi = rx->data[7] & 0x1FU;
	uint8_t d[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, rx->data[5], rx->data[6], rx->data[7]};
	uint8_t refused = DM22_UNKNOWN_DTC;

	if (control != DM22_CLEAR_PREVIOUS && control != DM22_CLEAR_ACTIVE) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(dtcs); i++) {
		struct dtc *dtc = &dtcs[i];

		if (dtc->spn != spn || dtc->fmi != fmi) {
			continue;
		}
		if (control == DM22_CLEAR_ACTIVE && !dtc->active) {
			refused = DM22_NOT_ACTIVE;
		} else if (control == DM22_CLEAR_PREVIOUS && (dtc->active || dtc->oc == 0U)) {
			refused = DM22_NOT_PREVIOUS_ACTIVE;
		} else {
			dtc->active = false;
			dtc->oc = 0;
			refused = 0;
		}
	}
	d[0] = control + (refused != 0U ? 2U : 1U);
	if (refused != 0U) {
		d[1] = refused;
	}
	zelos_j1939_set(&msgs[M_DM22], d, sizeof(d));
	zelos_j1939_send_to(&msgs[M_DM22], rx->sa);
}

/*
 * DM7 with the rationality test identifier: DM58 answers with the SPN's data
 * as its PG carries it, 0xFF past its length; all 0xFF for an SPN the node
 * has no such data for.
 */
static void rationality(const struct engine *e, const struct diag_rx *rx, uint32_t spn)
{
	uint8_t d[sizeof(dm58)];

	memset(d, 0xFF, sizeof(d));
	d[0] = TEST_RATIONALITY;
	d[1] = rx->data[1];
	d[2] = rx->data[2];
	d[3] = rx->data[3] | 0x1FU; /* the SPN's top bits; the rest ones */
	if (spn == SPN_ENGINE_SPEED) {
		sys_put_le16(ENC(eec1, engine_speed, e->rpm), &d[4]);
	} else if (spn == dtcs[DTC_COOLANT].spn) {
		d[4] = ENC(et1, engine_coolant_temp, e->coolant_c);
	}
	zelos_j1939_set(&msgs[M_DM58], d, sizeof(d));
	zelos_j1939_send_to(&msgs[M_DM58], rx->sa);
}

/*
 * DM7 commands a test; DM30 answers with its result: the SPN's value as its
 * PG carries it, against the limit its DTC sets at. A test the node lacks, or
 * oil pressure with the engine below idle, cannot be performed.
 */
static void command_test(const struct engine *e, const struct diag_rx *rx)
{
	const uint32_t spn = sys_get_le16(&rx->data[1]) | ((uint32_t)(rx->data[3] >> 5) << 16);
	uint8_t d[sizeof(dm30)];
	uint16_t value = TEST_CANNOT_RUN;
	uint16_t max = TEST_NO_LIMIT;
	uint16_t min = TEST_NO_LIMIT;

	if (rx->data[0] == TEST_RATIONALITY) {
		rationality(e, rx, spn);
		return;
	}
	memcpy(d, rx->data, 4);
	if (rx->data[0] == TEST_ALL && spn == dtcs[DTC_COOLANT].spn) {
		d[3] = (rx->data[3] & 0xE0U) | dtcs[DTC_COOLANT].fmi;
		value = ENC(et1, engine_coolant_temp, e->coolant_c);
		max = ENC(et1, engine_coolant_temp, HOT_ON_C);
	} else if (rx->data[0] == TEST_ALL && spn == dtcs[DTC_OIL].spn) {
		d[3] = (rx->data[3] & 0xE0U) | dtcs[DTC_OIL].fmi;
		if (e->rpm >= IDLE_RPM) {
			value = ENC(efl_p1, engine_oil_pressure, e->oil_kpa);
			min = ENC(efl_p1, engine_oil_pressure, OIL_LOW_KPA);
		}
	}
	sys_put_le16(SLOT_UNAVAILABLE, &d[4]);
	sys_put_le16(value, &d[6]);
	sys_put_le16(max, &d[8]);
	sys_put_le16(min, &d[10]);
	zelos_j1939_set(&msgs[M_DM30], d, sizeof(d));
	zelos_j1939_send_to(&msgs[M_DM30], rx->sa);
}

static uint8_t memory[MEM_SIZE];

/* One memory access at a time: the last DM14 wins. */
static struct {
	enum {
		MEM_IDLE,
		MEM_KEYING,  /* seed sent, waiting for the same request with the key */
		MEM_SEND,    /* read granted: DM16 goes on the next pass */
		MEM_SENDING, /* DM16 on its way: completed once it has gone */
		MEM_RECEIVE, /* write granted: waiting for DM16 */
	} state;
	uint8_t sa;
	uint8_t command;
	uint16_t len;
	uint32_t pointer;
	uint16_t seed;
} mem;

/* DM15 to the tool. Byte 2 bits 1 and 5 are ones. */
static void mem_answer(uint16_t len, uint8_t status, uint32_t error, uint16_t seed)
{
	const struct genset_dm15_t m = {
		.length_allowed = len & 0xFFU,
		.length_allowed_high = len >> 8,
		.status = status,
		.edc_parameter = error,
		.edcp_extension = error == MEM_NO_ERROR ? 0xFFU : MEM_EDCP_ERROR,
		.seed = seed,
	};
	uint8_t d[8];

	(void)genset_dm15_pack(d, &m, sizeof(d));
	d[1] |= 0x11U;
	zelos_j1939_set(&msgs[M_DM15], d, sizeof(d));
	zelos_j1939_send_to(&msgs[M_DM15], mem.sa);
}

static void mem_fail(uint32_t error)
{
	mem.state = MEM_IDLE;
	mem_answer(0, MEM_FAILED, error, MEM_NO_KEY);
}

/* DM14: a read or write of the memory, or the tool closing one. */
static void mem_request(const struct diag_rx *rx)
{
	const uint8_t command = (rx->data[1] >> 1) & 7U;
	const uint16_t len = rx->data[0] | ((uint16_t)(rx->data[1] >> 5) << 8);
	/* Pointer and its extension as one address. */
	const uint32_t pointer = sys_get_le32(&rx->data[2]);
	const uint16_t key = sys_get_le16(&rx->data[6]);
	const bool keyed = mem.state == MEM_KEYING && rx->sa == mem.sa && command == mem.command &&
			   len == mem.len && pointer == mem.pointer;

	mem.state = MEM_IDLE;
	mem.sa = rx->sa;
	if (command == MEM_COMPLETED || command == MEM_FAILED) {
		return;
	}
	if (command != MEM_READ && command != MEM_WRITE) {
		mem_fail(MEM_ERR_UNKNOWN);
	} else if (pointer >= MEM_SIZE) {
		mem_fail(MEM_ERR_ADDRESS);
	} else if (len == 0U || len > MEM_SIZE - pointer ||
		   len > (command == MEM_READ ? MEM_READ_MAX : MEM_WRITE_MAX)) {
		mem_fail(MEM_ERR_LENGTH);
	} else if (!keyed) {
		/* The key field is a user level here; whatever it is, a key is wanted. */
		mem.seed = (uint16_t)k_cycle_get_32();
		if (mem.seed == 0U || mem.seed == MEM_NO_KEY) {
			mem.seed = MEM_KEY;
		}
		mem.command = command;
		mem.len = len;
		mem.pointer = pointer;
		mem.state = MEM_KEYING;
		mem_answer(0, MEM_PROCEED, MEM_NO_ERROR, mem.seed);
	} else if (key != (mem.seed ^ MEM_KEY)) {
		mem_fail(MEM_ERR_KEY);
	} else {
		mem.state = command == MEM_READ ? MEM_SEND : MEM_RECEIVE;
		mem_answer(len, MEM_PROCEED, MEM_NO_ERROR, MEM_NO_KEY);
	}
}

/* DM16 from the tool: the bytes of the write it was granted. */
static void mem_write(const struct diag_rx *rx)
{
	if (mem.state != MEM_RECEIVE || rx->sa != mem.sa || rx->len == 0U) {
		return;
	}
	if (MIN(rx->data[0], rx->len - 1U) != mem.len) {
		mem_fail(MEM_ERR_LENGTH);
		return;
	}
	memcpy(&memory[mem.pointer], &rx->data[1], mem.len);
	mem.state = MEM_IDLE;
	mem_answer(0, MEM_COMPLETED, MEM_NO_ERROR, MEM_NO_KEY);
}

/*
 * A granted read, a pass at a time so each DM15 has gone before the next
 * replaces it: DM16 with the bytes, then DM15 completed once DM16 has gone.
 * DM16's first byte counts the data, or is 0xFF above 7 bytes, where it goes
 * by transport.
 */
static void mem_step(void)
{
	uint8_t d[sizeof(dm16)];

	if (mem.state == MEM_SEND) {
		memset(d, 0xFF, sizeof(d));
		d[0] = mem.len > 7U ? 0xFFU : (uint8_t)mem.len;
		memcpy(&d[1], &memory[mem.pointer], mem.len);
		zelos_j1939_set(&msgs[M_DM16], d, MAX(1U + mem.len, 8U));
		zelos_j1939_send_to(&msgs[M_DM16], mem.sa);
		mem.state = MEM_SENDING;
	} else if (mem.state == MEM_SENDING && !zelos_j1939_pending(&j1939, &msgs[M_DM16])) {
		mem.state = MEM_IDLE;
		mem_answer(0, MEM_COMPLETED, MEM_NO_ERROR, MEM_NO_KEY);
	}
}

/* What a tool asked since the last pass. Before the model's step, so a cleared fault that persists is back in this pass. */
static void diagnose(struct engine *e)
{
	struct diag_rx rx;

	mem_step();

	while (k_msgq_get(&genset_diag_msgq, &rx, K_NO_WAIT) == 0) {
		if (rx.pgn == PGN_DM3 || rx.pgn == PGN_DM11) {
			clear_dtcs(e, rx.pgn == PGN_DM11);
			/* J1939-73: acknowledged to the one who asked this node, not to a global request. */
			if (rx.to_us) {
				zelos_j1939_ack(&j1939, rx.sa, rx.pgn);
			}
		} else if (rx.pgn == PGN_DM16) {
			mem_write(&rx);
		} else if (rx.len < 8U) {
			continue;
		} else if (rx.pgn == PGN_DM22) {
			clear_one(&rx);
		} else if (rx.pgn == PGN_DM7) {
			command_test(e, &rx);
		} else if (rx.pgn == PGN_DM14) {
			mem_request(&rx);
		}
	}
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
		follow_tsc1(&engine, k_uptime_get());
		diagnose(&engine);
		step(&engine);
		freeze(&engine);
		publish(&engine);

		deadline += LOOP_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}

/*
 * The model, on a thread of its own so an image can run it beside other
 * nodes; main()'s priority, where it ran before, and room for the float
 * model, two dozen packs and a memory read on its way out.
 */
K_THREAD_DEFINE(genset_tid, 3072, run, NULL, NULL, NULL, 0, 0, SYS_FOREVER_MS);

int genset_start(void)
{
	if (zelos_can_subscribe(BENCH_VCU_COMMAND_FRAME_ID, &genset_vcu_msgq) < 0) {
		LOG_ERR("subscribe to VCU_Command failed");
		return -EIO;
	}
	/* Any destination: the address may move, and follow_tsc1 checks it. */
	if (zelos_can_subscribe_ext(PGN_TSC1 << 8, PF_MASK, &genset_tsc1_msgq) < 0) {
		LOG_ERR("subscribe to TSC1 failed");
		return -EIO;
	}

	for (size_t i = 0; i < MEM_SIZE; i++) {
		memory[i] = (uint8_t)(0xA0U + i);
	}
	{
		/* The frame of the coolant fault in the history: idling, warm. */
		static const uint8_t snapshot[FREEZE_DATA_LEN] = {0x20, 0x1C, 130, 100, 0};
		uint8_t dtc[4];

		put_dtc(dtc, &dtcs[DTC_COOLANT]);
		freeze_dm4(dtc, snapshot);
	}
	publish(&engine);
	if (zelos_j1939_start(&j1939) != 0) {
		return -EIO;
	}

	LOG_INF("genset up, NAME 0x%016llx", (unsigned long long)NAME);
	k_thread_start(genset_tid);

	return 0;
}
