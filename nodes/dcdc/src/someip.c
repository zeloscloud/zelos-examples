/*
 * The DC-DC as a SOME/IP service, as arxml/can-full.arxml describes it:
 * DcdcService 0x5A00, instance 1, version 1.0, on UDP 30509, eventgroup 1.
 *
 *   Event DcdcStatus 0x8001, every 100 ms, 26 bytes, big-endian: output V,
 *   output A, input V (NaN with no fresh BMS_Status), input A, temperature
 *   degC (float32 each), DcdcState (uint8: 0 Idle, 1 Converting, 2 Derating,
 *   as the AUTOSAR PDUs), the DCDC_Status Counter of the same sample (uint8),
 *   uptime ms (uint32).
 *   Field Setpoint: UDS DID 0x0200, uint16 10 mV. Getter 0x0001, notifier
 *   0x8002. No setter: writes go through UDS, behind security access.
 *
 * CommunicationControl (0x28) stops the event and the notifier, as it stops
 * DCDC_Status; SD and the getter go on.
 */

#include "someip.h"

#include <string.h>

#include <zelos/someip.h>
#include <zelos/uds.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "uds.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_ZELOS_UDS), "the Setpoint field is UDS's DID 0x0200");

enum { IDLE, CONVERTING, DERATING };

static struct k_spinlock lock;
static struct dcdc_state latest;
static uint8_t latest_state;
static uint8_t latest_counter;
static bool sampled;

void dcdc_someip_sample(const struct dcdc_state *state, bool derating, uint8_t counter)
{
	const uint8_t mode = derating ? DERATING : state->input_a > 0.0f ? CONVERTING : IDLE;
	k_spinlock_key_t key = k_spin_lock(&lock);

	latest = *state;
	latest_state = mode;
	latest_counter = counter;
	sampled = true;
	k_spin_unlock(&lock, key);
}

static uint8_t *put_f32(uint8_t *p, float v)
{
	uint32_t bits;

	memcpy(&bits, &v, sizeof(bits));
	sys_put_be32(bits, p);
	return p + 4;
}

static bool read_status(uint8_t *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const struct dcdc_state s = latest;
	const uint8_t mode = latest_state;
	const uint8_t counter = latest_counter;
	const bool have = sampled;

	k_spin_unlock(&lock, key);
	if (!have) {
		return false;
	}
	out = put_f32(out, s.output_v);
	out = put_f32(out, s.output_a);
	out = put_f32(out, s.input_v);
	out = put_f32(out, s.input_a);
	out = put_f32(out, s.temperature_c);
	out[0] = mode;
	out[1] = counter;
	sys_put_be32(s.uptime_ms, &out[2]);
	return true;
}

static void read_setpoint(uint8_t *out)
{
	sys_put_be16(dcdc_uds_setpoint(), out);
}

static bool may_notify(void)
{
	return zelos_uds_tx_enabled();
}

static const struct zelos_someip_event events[] = {
	{.id = 0x8001, .cycle_ms = 100, .size = 26, .read = read_status},
};

static const struct zelos_someip_field fields[] = {
	{.getter = 0x0001, .notifier = 0x8002, .size = 2, .read = read_setpoint},
};

static const struct zelos_someip_service service = {
	.service = 0x5A00,
	.instance = 0x0001,
	.major = 1,
	.minor = 0,
	.port = 30509,
	.eventgroup = 0x0001,
	.events = events,
	.event_count = ARRAY_SIZE(events),
	.fields = fields,
	.field_count = ARRAY_SIZE(fields),
	.may_notify = may_notify,
};

int dcdc_someip_start(void)
{
	return zelos_someip_offer(&service);
}
