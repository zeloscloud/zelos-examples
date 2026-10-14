/*
 * The DC-DC's AUTOSAR PDUs, built with `-S autosar`.
 *
 * Ids, layouts, scaling, timing, E2E and SecOC parameters come from
 * arxml/can-full.arxml through the generated autosar_pdus.{c,h}; this file
 * only maps the model's state onto the signals. A PDU the ARXML adds without a
 * build_<name>() here fails the build.
 *
 * Every frame carries one model sample (50 ms loop): the same values as a
 * DCDC_Status of that sample, to each signal's resolution. UDS
 * CommunicationControl stops these with the node's other normal messages.
 */

#include "autosar.h"

#include <zelos/autosar.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "autosar_pdus.h"

LOG_MODULE_REGISTER(dcdc_autosar, LOG_LEVEL_INF);

/*
 * SecOC key of DcdcSecured: the AES-128 example key of RFC 4493, public on
 * purpose. A demo key; real keys come from a key manager, never from the ARXML.
 */
static const uint8_t secoc_key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
				      0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};

/* DcdcState: one compu method in the ARXML, so the same codes in every PDU. */
enum {
	IDLE = AR_DCDC_OUTPUT_P1_DCDC_STATE_IDLE,
	CONVERTING = AR_DCDC_OUTPUT_P1_DCDC_STATE_CONVERTING,
	DERATING = AR_DCDC_OUTPUT_P1_DCDC_STATE_DERATING
};
#define SAME_STATES(PDU)                                                                           \
	BUILD_ASSERT(AR_##PDU##_DCDC_STATE_IDLE == IDLE &&                                         \
		     AR_##PDU##_DCDC_STATE_CONVERTING == CONVERTING &&                             \
		     AR_##PDU##_DCDC_STATE_DERATING == DERATING)
SAME_STATES(DCDC_HEALTH);
SAME_STATES(DCDC_MUX_STATIC);

/* The latest sample, written by the model's loop, read by the AUTOSAR thread. */
static struct k_spinlock lock;
static struct dcdc_state latest;
static uint8_t latest_state;
/* False until the first sample, and while CommunicationControl disables tx. */
static bool sending;

/* E2E counters and the SecOC freshness value, one per PDU. */
static uint8_t p1_counter;
static uint8_t p2_counter;
static uint8_t p5_counter;
static uint8_t freshness;

void dcdc_autosar_sample(const struct dcdc_state *state, bool derating, bool tx)
{
	const uint8_t mode = derating ? DERATING : state->input_a > 0.0f ? CONVERTING : IDLE;

	K_SPINLOCK(&lock) {
		latest = *state;
		latest_state = mode;
		sending = tx;
	}
}

/* This frame's sample, or false: nothing to send. */
static bool take(struct dcdc_state *s, uint8_t *mode)
{
	bool tx;

	K_SPINLOCK(&lock) {
		*s = latest;
		*mode = latest_state;
		tx = sending;
	}
	return tx;
}

/* --- E2E protected ------------------------------------------------------- */

static uint8_t build_dcdc_output_p1(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(data, &ar_dcdc_output_p1);
	zelos_ar_pack(data, &ar_dcdc_output_p1_dcdc_state, mode);
	zelos_ar_set(data, &ar_dcdc_output_p1_output_voltage, s.output_v);
	zelos_ar_set(data, &ar_dcdc_output_p1_output_current, s.output_a);
	zelos_ar_set(data, &ar_dcdc_output_p1_input_current, s.input_a);
	zelos_e2e_p01_protect(&ar_dcdc_output_p1_e2e, &p1_counter, data);
	return ar_dcdc_output_p1.length;
}

static uint8_t build_dcdc_thermal_p2(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(data, &ar_dcdc_thermal_p2);
	zelos_ar_set(data, &ar_dcdc_thermal_p2_temperature, s.temperature_c);
	zelos_ar_pack(data, &ar_dcdc_thermal_p2_derate_active,
		      mode == DERATING ? AR_DCDC_THERMAL_P2_DERATE_ACTIVE_ACTIVE
				       : AR_DCDC_THERMAL_P2_DERATE_ACTIVE_INACTIVE);
	zelos_ar_set(data, &ar_dcdc_thermal_p2_aux_current_limit, s.aux_limit_a);
	zelos_e2e_p02_protect(&ar_dcdc_thermal_p2_e2e, &p2_counter, data);
	return ar_dcdc_thermal_p2.length;
}

static uint8_t build_dcdc_power_p5(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(data, &ar_dcdc_power_p5);
	zelos_ar_set(data, &ar_dcdc_power_p5_output_power, s.output_v * s.output_a);
	zelos_ar_set(data, &ar_dcdc_power_p5_input_voltage, s.input_v); /* NaN: Unknown */
	zelos_ar_set(data, &ar_dcdc_power_p5_setpoint_voltage, s.setpoint_v);
	zelos_e2e_p05_protect(&ar_dcdc_power_p5_e2e, &p5_counter, data);
	return ar_dcdc_power_p5.length;
}

/* --- Contained in DcdcContainer, one per frame ---------------------------- */

static uint8_t build_dcdc_rail(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t pdu[CAN_MAX_DLEN];

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(pdu, &ar_dcdc_rail);
	zelos_ar_set(pdu, &ar_dcdc_rail_output_voltage, s.output_v);
	zelos_ar_set(pdu, &ar_dcdc_rail_output_current, s.output_a);
	return zelos_ar_contain(data, AR_DCDC_RAIL_HEADER_ID, pdu, ar_dcdc_rail.length);
}

static uint8_t build_dcdc_input(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t pdu[CAN_MAX_DLEN];

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(pdu, &ar_dcdc_input);
	zelos_ar_set(pdu, &ar_dcdc_input_input_current, s.input_a);
	zelos_ar_set(pdu, &ar_dcdc_input_input_voltage, s.input_v);
	return zelos_ar_contain(data, AR_DCDC_INPUT_HEADER_ID, pdu, ar_dcdc_input.length);
}

static uint8_t build_dcdc_health(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t pdu[CAN_MAX_DLEN];

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(pdu, &ar_dcdc_health);
	zelos_ar_set(pdu, &ar_dcdc_health_temperature, s.temperature_c);
	zelos_ar_pack(pdu, &ar_dcdc_health_dcdc_state, mode);
	/* Seconds since boot, wrapping at 16 bits. */
	zelos_ar_pack(pdu, &ar_dcdc_health_uptime_s, (s.uptime_ms / 1000U) & 0xFFFFU);
	return zelos_ar_contain(data, AR_DCDC_HEALTH_HEADER_ID, pdu, ar_dcdc_health.length);
}

/* --- DcdcMux: the static part, and one dynamic part per frame -------------- */

/* The multiplexed PDU with its static part in place. */
static void mux_static(uint8_t *data, const struct dcdc_state *s, uint8_t mode)
{
	uint8_t part[CAN_MAX_DLEN];

	zelos_ar_init(data, &ar_dcdc_mux);
	zelos_ar_init(part, &ar_dcdc_mux_static);
	zelos_ar_pack(part, &ar_dcdc_mux_static_dcdc_state, mode);
	zelos_ar_set(part, &ar_dcdc_mux_static_output_voltage, s->output_v);
	zelos_ar_merge(data, part, ar_dcdc_mux_static_segments,
		       ARRAY_SIZE(ar_dcdc_mux_static_segments));
}

static uint8_t build_dcdc_mux_currents(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t part[CAN_MAX_DLEN];

	if (!take(&s, &mode)) {
		return 0;
	}
	mux_static(data, &s, mode);
	zelos_ar_init(part, &ar_dcdc_mux_currents);
	zelos_ar_pack(part, &ar_dcdc_mux_selector, AR_DCDC_MUX_CURRENTS_SELECTOR);
	zelos_ar_set(part, &ar_dcdc_mux_currents_output_current, s.output_a);
	zelos_ar_set(part, &ar_dcdc_mux_currents_input_current, s.input_a);
	zelos_ar_merge(data, part, ar_dcdc_mux_dynamic_segments,
		       ARRAY_SIZE(ar_dcdc_mux_dynamic_segments));
	return ar_dcdc_mux.length;
}

static uint8_t build_dcdc_mux_limits(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t part[CAN_MAX_DLEN];

	if (!take(&s, &mode)) {
		return 0;
	}
	mux_static(data, &s, mode);
	zelos_ar_init(part, &ar_dcdc_mux_limits);
	zelos_ar_pack(part, &ar_dcdc_mux_selector, AR_DCDC_MUX_LIMITS_SELECTOR);
	zelos_ar_set(part, &ar_dcdc_mux_limits_aux_current_limit, s.aux_limit_a);
	zelos_ar_set(part, &ar_dcdc_mux_limits_setpoint_voltage, s.setpoint_v);
	zelos_ar_merge(data, part, ar_dcdc_mux_dynamic_segments,
		       ARRAY_SIZE(ar_dcdc_mux_dynamic_segments));
	return ar_dcdc_mux.length;
}

/* --- DcdcSecured: SecOC profile 1 ----------------------------------------- */

BUILD_ASSERT(AR_DCDC_SECURED_LENGTH <= CAN_MAX_DLEN);

static uint8_t build_dcdc_secured_payload(uint8_t *data)
{
	struct dcdc_state s;
	uint8_t mode;
	uint8_t auth[2 + CAN_MAX_DLEN + AR_DCDC_SECURED_FRESHNESS_BITS / 8];
	uint8_t mac[16];
	size_t len;

	if (!take(&s, &mode)) {
		return 0;
	}
	zelos_ar_init(data, &ar_dcdc_secured_payload);
	zelos_ar_set(data, &ar_dcdc_secured_payload_output_voltage, s.output_v);
	zelos_ar_set(data, &ar_dcdc_secured_payload_input_current, s.input_a);

	len = zelos_secoc_auth_data(auth, AR_DCDC_SECURED_DATA_ID, data,
				    ar_dcdc_secured_payload.length, freshness,
				    AR_DCDC_SECURED_FRESHNESS_BITS);
	if (zelos_autosar_cmac(secoc_key, auth, len, mac) != 0) {
		/* No authenticator, no frame: a gap on the bus, never a bad MAC. */
		LOG_ERR_RATELIMIT("SecOC CMAC failed");
		return 0;
	}
	zelos_secoc_trailer(data, ar_dcdc_secured_payload.length, freshness,
			    AR_DCDC_SECURED_FRESHNESS_TX_BITS, mac, AR_DCDC_SECURED_AUTH_TX_BITS);
	/* After the PDU is built (SWS_SecOC_00031 step 5); 8 bits wrap. */
	freshness++;
	return AR_DCDC_SECURED_LENGTH;
}

/* --- Schedule -------------------------------------------------------------- */

#define ENTRY(name, NAME)                                                                          \
	{AR_##NAME##_ID, AR_##NAME##_PERIOD_MS, AR_##NAME##_OFFSET_MS, build_##name},

static const struct zelos_ar_tx schedule[] = {AR_TX_PDUS(ENTRY)};

BUILD_ASSERT(ARRAY_SIZE(schedule) <= CONFIG_ZELOS_AUTOSAR_TX_MAX,
	     "raise CONFIG_ZELOS_AUTOSAR_TX_MAX");

int dcdc_autosar_start(void)
{
	return zelos_autosar_start(schedule, ARRAY_SIZE(schedule));
}
