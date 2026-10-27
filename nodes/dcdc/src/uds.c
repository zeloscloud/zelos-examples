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
 *   0x0111 Ethernet, with lib/eth only, 28 bytes: link up (0/1), speed Mb/s
 *          (0, 10, 100), full duplex (0/1), IPv4 (enum zelos_eth_ipv4: 0 none,
 *          1 probing, 2 up, 3 conflict, 4 contested), MAC (6), then uint32 RX packets, TX
 *          packets, RX dropped (struct zelos_eth_status), then DoIP testers
 *          activated and SOME/IP subscribers (uint8 each), then uint32 frames
 *          the MAC dropped (FIFO full, no descriptor): the loss count
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
 * DTCs, from the model, each tested every 50 ms cycle (SAE J2012 generic
 * numbers, failure type 00):
 *   0 0x0A9400 P0A94 DC/DC converter performance: temperature above 70 degC
 *   1 0x0A0900 P0A09 DC/DC converter status circuit low: pack voltage below
 *              250 V (untested without one)
 *   2 0x0A0800 P0A08 DC/DC converter status circuit: output current above 300 A
 *   3 0xC11100 U0111 lost communication with the battery energy control
 *              module: BMS_Limits missing for 500 ms (untested while 0x28
 *              disables Rx)
 *   4 0x062F00 P062F internal control module EEPROM error: the permanent-DTC
 *              store failed (the server's, include/zelos/uds.h; only once set)
 * Snapshot record 0x01: 0x0101, 0x0102, 0x0103, 0x0104, 0x0105.
 *
 * OBD-II (SAE J1979), on the same identifiers. PIDs, each from a DID above so
 * the freeze frame (service 0x02) reports it from the snapshot:
 *   0x1F run time since engine start = uptime, s, uint16 saturating
 *   0x42 control module voltage = output voltage, ((A*256)+B)/1000 V
 *   0x46 ambient air temperature = temperature, A-40 degC
 *   0x5B hybrid battery pack remaining life = pack voltage from 250 V (0 %)
 *        to 400 V (100 %), A*100/255 %; 0 while unknown
 * plus the core's 0x01 (MIL, confirmed count, comprehensive components
 * supported and complete) and 0x1C (0x01, OBD-II CARB). Service 0x09: VIN =
 * 0xF190, CALID "DCDC-1.0.0", CVN = CRC-32 of the download window, ECU name
 * "DCDC". OBDonUDS (J1979-2) DIDs 0xF810, 0xF400 + PID, 0xF501,
 * 0xF802/0xF804/0xF806/0xF80A. DTC lifecycle (two-cycle confirmation, MIL,
 * aging, permanent DTCs in flash): include/zelos/uds.h; one operation cycle
 * is one boot, so an ECUReset ends it.
 */

#include "uds.h"

#include <math.h>
#include <string.h>

#include <zelos/can.h>
#include <zelos/doip.h>
#include <zelos/eth.h>
#include <zelos/someip.h>
#include <zelos/uds.h>

#include <zephyr/kernel.h>
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
/* PID 0x5B: pack life, 0 % at the undervoltage limit, 100 % at a full pack. */
#define PACK_FULL_V 400.0f
#define ECU_NAME "DCDC"

#define NRC_OUT_OF_RANGE 0x31

enum { DTC_OVERTEMPERATURE, DTC_UNDERVOLTAGE, DTC_OVERCURRENT, DTC_BMS_LOST, DTC_COUNT };

static const uint32_t dtcs[DTC_COUNT] = {
	[DTC_OVERTEMPERATURE] = 0x0A9400,
	[DTC_UNDERVOLTAGE] = 0x0A0900,
	[DTC_OVERCURRENT] = 0x0A0800,
	[DTC_BMS_LOST] = 0xC11100,
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

/* Written by UDS, read by DoIP's thread too (vehicle identification). */
static struct k_spinlock vin_lock;

static void read_vin(uint8_t *out)
{
	k_spinlock_key_t key = k_spin_lock(&vin_lock);

	memcpy(out, vin, VIN_LEN);
	k_spin_unlock(&vin_lock, key);
}

static uint8_t write_vin(const uint8_t *in)
{
	k_spinlock_key_t key;

	for (size_t i = 0; i < VIN_LEN; i++) {
		if (in[i] < 0x20 || in[i] > 0x7E) {
			return NRC_OUT_OF_RANGE;
		}
	}
	key = k_spin_lock(&vin_lock);
	memcpy(vin, in, VIN_LEN);
	k_spin_unlock(&vin_lock, key);
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

#if defined(CONFIG_ZELOS_ETH)
static void read_eth(uint8_t *out)
{
	const struct zelos_eth_status st = zelos_eth_status();

	out[0] = st.link;
	out[1] = st.speed_mbps;
	out[2] = st.full_duplex;
	out[3] = st.ipv4;
	memcpy(&out[4], st.mac, sizeof(st.mac));
	sys_put_be32(st.rx_packets, &out[10]);
	sys_put_be32(st.tx_packets, &out[14]);
	sys_put_be32(st.rx_errors, &out[18]);
#if defined(CONFIG_ZELOS_DOIP)
	out[22] = zelos_doip_testers();
#else
	out[22] = 0;
#endif
#if defined(CONFIG_ZELOS_SOMEIP)
	out[23] = zelos_someip_subscribers();
#else
	out[23] = 0;
#endif
	sys_put_be32(st.rx_dropped, &out[24]);
}
#endif

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
#if defined(CONFIG_ZELOS_ETH)
	{0x0111, 28, read_eth, NULL},
#endif
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

static uint8_t self_test(const uint8_t *option, uint8_t *result)
{
	ARG_UNUSED(option);
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

/* J1979 PIDs, each a formula over one DID's bytes (big-endian, as read above). */
static void pid_run_time(const uint8_t *uptime_ms, uint8_t *out)
{
	sys_put_be16((uint16_t)MIN(sys_get_be32(uptime_ms) / 1000U, UINT16_MAX), out);
}

static void pid_module_voltage(const uint8_t *volts_f32, uint8_t *out)
{
	uint32_t bits = sys_get_be32(volts_f32);
	float v;

	memcpy(&v, &bits, sizeof(v));
	sys_put_be16((uint16_t)scaled(v, 0.001f, 0, UINT16_MAX), out);
}

static void pid_ambient_temperature(const uint8_t *celsius_i8, uint8_t *out)
{
	out[0] = (uint8_t)CLAMP((int8_t)celsius_i8[0] + 40, 0, UINT8_MAX);
}

static void pid_pack_life(const uint8_t *pack_01v, uint8_t *out)
{
	uint16_t raw = sys_get_be16(pack_01v);
	float life = (raw * 0.1f - UNDERVOLTAGE_V) / (PACK_FULL_V - UNDERVOLTAGE_V);

	out[0] = raw == UINT16_MAX ? 0
				   : (uint8_t)scaled(CLAMP(life, 0.0f, 1.0f), 1.0f / 255, 0, 255);
}

static const struct zelos_uds_pid pids[] = {
	{0x1F, 2, 0x0104, pid_run_time},
	{0x42, 2, 0x0101, pid_module_voltage},
	{0x46, 1, 0x0103, pid_ambient_temperature},
	{0x5B, 1, 0x0105, pid_pack_life},
};

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
	.pids = pids,
	.pid_count = ARRAY_SIZE(pids),
	.calid = SW_VERSION,
	.ecu_name = ECU_NAME,
};

#if defined(CONFIG_ZELOS_DOIP)
static const struct zelos_doip_config doip = {
	.logical_address = CONFIG_ZELOS_DOIP_LOGICAL_ADDRESS,
	.functional_address = CONFIG_ZELOS_DOIP_FUNCTIONAL_ADDRESS,
	.vin = read_vin,
};
#endif

int dcdc_uds_start(void)
{
	int err = zelos_uds_start(&config);

#if defined(CONFIG_ZELOS_DOIP)
	if (err == 0) {
		err = zelos_doip_start(&doip);
	}
#endif
	return err;
}

uint16_t dcdc_uds_setpoint(void)
{
	return setpoint;
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
