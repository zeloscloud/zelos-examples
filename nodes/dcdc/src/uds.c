/*
 * What the DC-DC exposes over UDS, built with `-S uds`. The contract the UDS
 * extension's demo ECU mirrors; testing/suites/uds checks both.
 *
 * Data identifiers, big-endian:
 *   0xF186 active session, 1 byte          0xF187 part number "ZLS-DCDC-0001"
 *   0xF18A supplier "ZELOS"                0xF18C serial number "ZD00000001"
 *   0xF190 VIN, 17 ASCII, "1ZLSDCDC0BENCH001"; written in extended with level
 *          0x01, printable ASCII only, RAM until reset
 *   0xF191 hardware version "NUCLEO-H753ZI" 0xF195 software version "DCDC-1.0.0"
 *   0x0101 output voltage, float32 V       0x0102 output current, int16 0.1 A
 *   0x0103 temperature, int8 degC          0x0104 uptime, uint32 ms
 *   0x0105 input (pack) voltage, uint16 0.1 V, 0xFFFF when unknown
 *   0x0106 input current, uint16 0.01 A
 *   0x0110 CAN statistics, 6 x uint32 (see struct zelos_can_stats): sent,
 *          overflow, deferred, tx_error, peak, rx_overrun. The node's, so in
 *          a combined image every protocol's
 *   0x0200 output voltage setpoint, uint16 10 mV, 1380 at power-on, 1000..1600;
 *          written in extended with level 0x01. The model regulates to it.
 * I/O control 0x0300: the same setpoint, overridden: return, reset to 1380,
 * freeze, short-term adjustment (1000..1600).
 * Memory, by address (0x23, 0x3D):
 *   0x00010000, 32 bytes, read-only: struct dcdc_state (src/state.h), little-endian
 *   0x00020000, 8 bytes, writable in extended with level 0x01: demand A and
 *              slew rate A/s, float32 little-endian (struct dcdc_calibration)
 * Routine 0x0201: self-test, extended, about 2 s. Result: one byte, bit n set
 * while DTC n below is failing; 0x00 passed.
 * DTCs, from the model, each tested every 50 ms cycle:
 *   0 0x1A0098 overtemperature: temperature above 70 degC
 *   1 0x1A0116 input undervoltage: pack voltage below 250 V (untested without one)
 *   2 0x1A0219 output overcurrent: output current above 300 A
 *   3 0xC11187 BMS_Limits missing for 500 ms (untested while 0x28 disables Rx)
 * Snapshot record 0x01: 0x0101, 0x0102, 0x0103, 0x0104, 0x0105.
 */

#include "uds.h"

#include <math.h>
#include <string.h>

#include <zelos/can.h>
#include <zelos/uds.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "calibration.h"

#define PART "ZLS-DCDC-0001"
#define SUPPLIER "ZELOS"
#define SERIAL "ZD00000001"
#define VIN_DEFAULT "1ZLSDCDC0BENCH001"
#define HW_VERSION "NUCLEO-H753ZI"
#define SW_VERSION "DCDC-1.0.0"
#define VIN_LEN (sizeof(VIN_DEFAULT) - 1)

#define SETPOINT_DEFAULT 1380U
#define SETPOINT_MIN 1000U
#define SETPOINT_MAX 1600U

#define OVERTEMPERATURE_C 70.0f
#define UNDERVOLTAGE_V 250.0f
#define OVERCURRENT_A 300.0f

#define NRC_OUT_OF_RANGE 0x31

enum { DTC_OVERTEMPERATURE, DTC_UNDERVOLTAGE, DTC_OVERCURRENT, DTC_BMS_LOST, DTC_COUNT };

static const uint32_t dtcs[DTC_COUNT] = {
	[DTC_OVERTEMPERATURE] = 0x1A0098,
	[DTC_UNDERVOLTAGE] = 0x1A0116,
	[DTC_OVERCURRENT] = 0x1A0219,
	[DTC_BMS_LOST] = 0xC11187,
};

/* This cycle's state, copied out of main(); also the read-only memory window. */
static struct dcdc_state state;
/* Bit n: DTC n failed its last test. */
static uint8_t failing;

static char vin[] = VIN_DEFAULT;
/* 10 mV. The override is the tester's, -1 while the ECU is in control. */
static uint16_t setpoint = SETPOINT_DEFAULT;
static int32_t override = -1;

static uint16_t output(void)
{
	return override < 0 ? setpoint : (uint16_t)override;
}

#define READ_ASCII(name, text)                                                                     \
	static void name(uint8_t *out)                                                             \
	{                                                                                          \
		memcpy(out, text, sizeof(text) - 1);                                               \
	}

READ_ASCII(read_part, PART)
READ_ASCII(read_supplier, SUPPLIER)
READ_ASCII(read_serial, SERIAL)
READ_ASCII(read_hw_version, HW_VERSION)
READ_ASCII(read_sw_version, SW_VERSION)

static void read_vin(uint8_t *out)
{
	memcpy(out, vin, VIN_LEN);
}

static uint8_t write_vin(const uint8_t *in)
{
	for (size_t i = 0; i < VIN_LEN; i++) {
		if (in[i] < 0x20 || in[i] > 0x7E) {
			return NRC_OUT_OF_RANGE;
		}
	}
	memcpy(vin, in, VIN_LEN);
	return 0;
}

/* Rounded to steps of `step`, saturating at the type's range. */
static int32_t scaled(float value, float step, int32_t lo, int32_t hi)
{
	float raw = roundf(value / step);

	return !(raw >= lo) ? lo : (raw > hi ? hi : (int32_t)raw);
}

static void read_output_v(uint8_t *out)
{
	uint32_t bits;

	memcpy(&bits, &state.output_v, sizeof(bits));
	sys_put_be32(bits, out);
}

static void read_output_a(uint8_t *out)
{
	sys_put_be16((uint16_t)scaled(state.output_a, 0.1f, INT16_MIN, INT16_MAX), out);
}

static void read_temperature(uint8_t *out)
{
	out[0] = (uint8_t)scaled(state.temperature_c, 1.0f, INT8_MIN, INT8_MAX);
}

static void read_uptime(uint8_t *out)
{
	sys_put_be32(state.uptime_ms, out);
}

static void read_input_v(uint8_t *out)
{
	sys_put_be16(isnan(state.input_v) ? UINT16_MAX
					  : (uint16_t)scaled(state.input_v, 0.1f, 0, UINT16_MAX - 1),
		     out);
}

static void read_input_a(uint8_t *out)
{
	sys_put_be16((uint16_t)scaled(state.input_a, 0.01f, 0, UINT16_MAX), out);
}

static void read_setpoint(uint8_t *out)
{
	sys_put_be16(setpoint, out);
}

static bool in_range(uint16_t v)
{
	return v >= SETPOINT_MIN && v <= SETPOINT_MAX;
}

static uint8_t write_setpoint(const uint8_t *in)
{
	uint16_t v = sys_get_be16(in);

	if (!in_range(v)) {
		return NRC_OUT_OF_RANGE;
	}
	setpoint = v;
	return 0;
}

static void read_can_stats(uint8_t *out)
{
	const struct zelos_can_stats st = zelos_can_stats();
	const uint32_t v[] = {st.sent, st.overflow, st.deferred, st.tx_error, st.peak, st.rx_overrun};

	for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
		sys_put_be32(v[i], &out[4 * i]);
	}
}

static const struct zelos_uds_did dids[] = {
	{0xF187, sizeof(PART) - 1, read_part, NULL},
	{0xF18A, sizeof(SUPPLIER) - 1, read_supplier, NULL},
	{0xF18C, sizeof(SERIAL) - 1, read_serial, NULL},
	{0xF190, VIN_LEN, read_vin, write_vin},
	{0xF191, sizeof(HW_VERSION) - 1, read_hw_version, NULL},
	{0xF195, sizeof(SW_VERSION) - 1, read_sw_version, NULL},
	{0x0101, 4, read_output_v, NULL},
	{0x0102, 2, read_output_a, NULL},
	{0x0103, 1, read_temperature, NULL},
	{0x0104, 4, read_uptime, NULL},
	{0x0105, 2, read_input_v, NULL},
	{0x0106, 2, read_input_a, NULL},
	{0x0110, 24, read_can_stats, NULL},
	{0x0200, 2, read_setpoint, write_setpoint},
};

static void read_output(uint8_t *out)
{
	sys_put_be16(output(), out);
}

static uint8_t control_output(uint8_t param, const uint8_t *in)
{
	switch (param) {
	case ZELOS_UDS_IO_RETURN:
		override = -1;
		break;
	case ZELOS_UDS_IO_RESET:
		override = SETPOINT_DEFAULT;
		break;
	case ZELOS_UDS_IO_FREEZE:
		override = output();
		break;
	default:
		if (!in_range(sys_get_be16(in))) {
			return NRC_OUT_OF_RANGE;
		}
		override = sys_get_be16(in);
		break;
	}
	return 0;
}

static const struct zelos_uds_io ios[] = {
	{0x0300, 2, read_output, control_output},
};

static uint8_t self_test(uint8_t *result)
{
	result[0] = failing;
	return 1;
}

static const struct zelos_uds_routine routines[] = {
	{0x0201, ZELOS_UDS_SESSION_EXTENDED, 2000, self_test},
};

static const struct zelos_uds_memory memories[] = {
	{0x00010000, sizeof(state), &state, false},
	{0x00020000, sizeof(dcdc_cal), &dcdc_cal, true},
};

static const uint16_t snapshot_dids[] = {0x0101, 0x0102, 0x0103, 0x0104, 0x0105};

static const struct zelos_uds_config config = {
	.dids = dids,
	.did_count = ARRAY_SIZE(dids),
	.ios = ios,
	.io_count = ARRAY_SIZE(ios),
	.routines = routines,
	.routine_count = ARRAY_SIZE(routines),
	.memories = memories,
	.memory_count = ARRAY_SIZE(memories),
	.dtcs = dtcs,
	.dtc_count = ARRAY_SIZE(dtcs),
	.snapshot_dids = snapshot_dids,
	.snapshot_did_count = ARRAY_SIZE(snapshot_dids),
	.write = dcdc_cal_write,
};

int dcdc_uds_start(void)
{
	return zelos_uds_start(&config);
}

float dcdc_uds_output_v(void)
{
	return output() * 0.01f;
}

static void test(int dtc, bool failed)
{
	WRITE_BIT(failing, dtc, failed);
	zelos_uds_dtc(dtc, failed);
}

void dcdc_uds_sample(const struct dcdc_state *now, bool bms_monitored, bool bms_lost)
{
	state = *now;

	test(DTC_OVERTEMPERATURE, now->temperature_c > OVERTEMPERATURE_C);
	test(DTC_OVERCURRENT, now->output_a > OVERCURRENT_A);
	if (!isnan(now->input_v)) {
		test(DTC_UNDERVOLTAGE, now->input_v < UNDERVOLTAGE_V);
	}
	if (bms_monitored) {
		test(DTC_BMS_LOST, bms_lost);
	}
}
