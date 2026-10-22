/*
 * The SOME/IP core driven by hand: datagrams in, messages out, time injected.
 * Layouts from the PRS (SOME/IP header, SD entries and the IPv4 endpoint option).
 */

#include <zelos/someip.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define SELF 0x0A01014BU /* 10.1.1.75 */
#define PEER 0x0A010101U /* 10.1.1.1 */

static uint8_t value[2] = {0x05, 0x64};
static bool tx = true;

static bool read_event(uint8_t *out)
{
	out[0] = 0xAB;
	return true;
}

static void read_field(uint8_t *out)
{
	memcpy(out, value, sizeof(value));
}

static bool may_notify(void)
{
	return tx;
}

static const struct zelos_someip_event events[] = {{0x8001, 100, 1, read_event}};
static const struct zelos_someip_field fields[] = {{0x0001, 0x8002, 2, read_field}};
static const struct zelos_someip_service svc = {
	.service = 0x5A00, .instance = 1, .major = 1, .minor = 0, .port = 30509,
	.eventgroup = 1, .events = events, .event_count = 1, .fields = fields,
	.field_count = 1, .may_notify = may_notify,
};

/* What the core sent: the last SD message and service message, and counts. */
static uint8_t sd_out[ZELOS_SOMEIP_MESSAGE_MAX], out[ZELOS_SOMEIP_MESSAGE_MAX];
static size_t sd_len, out_len;
static uint32_t sd_to;
static int sd_sent, sent;
static uint16_t methods[32];

static void fake_send_sd(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(port);
	memcpy(sd_out, msg, len);
	sd_len = len;
	sd_to = addr;
	sd_sent++;
}

static void fake_send(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);
	zassert_equal(addr, PEER);
	zassert_equal(port, 40000);
	memcpy(out, msg, len);
	out_len = len;
	methods[sent++ % ARRAY_SIZE(methods)] = sys_get_be16(&msg[2]);
}

static uint32_t fixed_random(void)
{
	return 0; /* the shortest delays */
}

static const struct zelos_someip_ops ops = {fake_send_sd, fake_send, fixed_random};
static struct zelos_someip_core sip;
static int64_t now;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	sd_sent = sent = 0;
	tx = true;
	value[1] = 0x64;
	now = 1000;
	zelos_someip_core_init(&sip, &svc, &ops, NULL);
	zelos_someip_core_up(&sip, SELF, now);
}

/* An SD message from the peer with one 16-byte entry and, if port, its endpoint option. */
static void sd_in(const uint8_t entry[16], uint16_t port)
{
	uint8_t m[16 + 8 + 16 + 4 + 12] = {0xFF, 0xFF, 0x81, 0x00};
	size_t opts = port ? 12 : 0;
	size_t len = 16 + 8 + 16 + 4 + opts;

	sys_put_be32(len - 8, &m[4]);
	m[12] = 1;
	m[13] = 1;
	m[14] = 0x02;
	m[16] = 0xC0;
	sys_put_be32(16, &m[20]);
	memcpy(&m[24], entry, 16);
	sys_put_be32(opts, &m[40]);
	if (port) {
		const uint8_t opt[12] = {0, 9, 0x04, 0, 10, 1, 1, 1, 0, 0x11, port >> 8, port & 0xFF};

		memcpy(&m[44], opt, 12);
	}
	zelos_someip_core_sd(&sip, PEER, 30490, m, len, now);
}

static void subscribe(uint16_t eventgroup, uint32_t ttl)
{
	uint8_t e[16] = {0x06, 0, 0, 0x10, 0x5A, 0x00, 0x00, 0x01, 1, ttl >> 16, ttl >> 8, ttl,
			 0, 0, eventgroup >> 8, eventgroup & 0xFF};

	sd_in(e, 40000);
}

ZTEST(someip, test_offer_phases)
{
	/* Initial wait 10 ms (random 0), then 100, 200, 400 ms apart, then every second. */
	const int64_t at[] = {10, 110, 310, 710, 1710, 2710};

	for (size_t i = 0; i < ARRAY_SIZE(at); i++) {
		zelos_someip_core_tick(&sip, now + at[i] - 1);
		zassert_equal(sd_sent, i, "early at %d", (int)i);
		zelos_someip_core_tick(&sip, now + at[i]);
		zassert_equal(sd_sent, i + 1, "late at %d", (int)i);
	}
	/* SD header, session 6, Reboot + Unicast; one offer entry, TTL 3; UDP 10.1.1.75:30509. */
	const uint8_t want[] = {0xFF, 0xFF, 0x81, 0x00, 0, 0, 0, 48, 0, 0, 0, 6, 1, 1, 2, 0,
				0xC0, 0, 0, 0, 0, 0, 0, 16,
				0x01, 0, 0, 0x10, 0x5A, 0x00, 0, 1, 1, 0, 0, 3, 0, 0, 0, 0,
				0, 0, 0, 12, 0, 9, 0x04, 0, 10, 1, 1, 75, 0, 0x11, 0x77, 0x2D};

	zassert_equal(sd_len, sizeof(want));
	zassert_mem_equal(sd_out, want, sizeof(want));
	zassert_equal(sd_to, 0, "multicast");
}

ZTEST(someip, test_session_wrap_clears_reboot)
{
	sip.session_mc = 0xFFFE;
	zelos_someip_core_tick(&sip, now + 10);
	zassert_equal(sys_get_be16(&sd_out[10]), 0xFFFF);
	zassert_equal(sd_out[16] & 0x80, 0x80);
	zelos_someip_core_tick(&sip, now + 110);
	zassert_equal(sys_get_be16(&sd_out[10]), 1);
	zassert_equal(sd_out[16] & 0x80, 0);
}

ZTEST(someip, test_subscription)
{
	/* Ack with its TTL, the field's value at once, then events every 100 ms. */
	subscribe(1, 3);
	zassert_equal(sd_out[24], 0x07);
	zassert_equal(sys_get_be24(&sd_out[24 + 9]), 3);
	zassert_equal(sd_to, PEER);
	zassert_equal(sent, 1);
	zassert_equal(methods[0], 0x8002);
	zassert_mem_equal(&out[16], value, 2);
	zassert_equal(zelos_someip_core_subscribers(&sip), 1);
	zelos_someip_core_tick(&sip, now);
	zelos_someip_core_tick(&sip, now + 99);
	zelos_someip_core_tick(&sip, now + 100);
	zassert_equal(sent, 3);
	zassert_equal(methods[2], 0x8001);
	zassert_equal(out[14], 0x02, "notification");

	/* The field changes: notified. 0x28: nothing, then the change once back. */
	value[1] = 0x65;
	zelos_someip_core_tick(&sip, now + 150);
	zassert_equal(methods[sent - 1], 0x8002);
	tx = false;
	value[1] = 0x66;
	sent = 0;
	zelos_someip_core_tick(&sip, now + 300);
	zassert_equal(sent, 0);
	tx = true;
	zelos_someip_core_tick(&sip, now + 310);
	zassert_equal(methods[0], 0x8002);
	zassert_equal(out[17], 0x66);

	/* TTL ends: no more events. */
	sent = 0;
	zelos_someip_core_tick(&sip, now + 3000);
	zassert_equal(zelos_someip_core_subscribers(&sip), 0);
	zelos_someip_core_tick(&sip, now + 3200);
	zassert_equal(sent, 0);

	/* Unknown eventgroup: Nack (TTL 0). StopSubscribe: none kept, no answer. */
	subscribe(9, 3);
	zassert_equal(sys_get_be24(&sd_out[24 + 9]), 0);
	zassert_equal(zelos_someip_core_subscribers(&sip), 0);
	subscribe(1, 3);
	sd_sent = 0;
	subscribe(1, 0);
	zassert_equal(sd_sent, 0);
	zassert_equal(zelos_someip_core_subscribers(&sip), 0);
}

static void call(uint16_t service, uint16_t method, uint8_t version, uint8_t type,
		 size_t payload)
{
	uint8_t m[20] = {service >> 8, service & 0xFF, method >> 8, method & 0xFF};

	sys_put_be32(8 + payload, &m[4]);
	m[8] = 0x12;
	m[9] = 0x34;
	m[10] = 0x00;
	m[11] = 0x07;
	m[12] = 1;
	m[13] = version;
	m[14] = type;
	out_len = 0;
	zelos_someip_core_request(&sip, PEER, 40000, m, 16 + payload);
}

ZTEST(someip, test_getter_and_errors)
{
	const uint8_t want[] = {0x5A, 0x00, 0x00, 0x01, 0, 0, 0, 10, 0x12, 0x34, 0x00, 0x07,
				1, 1, 0x80, 0x00, 0x05, 0x64};

	call(0x5A00, 0x0001, 1, 0x00, 0);
	zassert_equal(out_len, sizeof(want));
	zassert_mem_equal(out, want, sizeof(want));
	call(0x5A00, 0x0042, 1, 0x00, 0);
	zassert_equal(out[14], 0x81);
	zassert_equal(out[15], 0x03);
	call(0x5A00, 0x0001, 2, 0x00, 0);
	zassert_equal(out[15], 0x08);
	call(0x1234, 0x0001, 1, 0x00, 0);
	zassert_equal(out[15], 0x02);
	call(0x5A00, 0x0001, 1, 0x00, 4);
	zassert_equal(out[15], 0x09);
	call(0x5A00, 0x0001, 1, 0x01, 0);
	zassert_equal(out_len, 0, "no return");
}

ZTEST(someip, test_find_is_answered_unicast)
{
	const uint8_t e[16] = {0x00, 0, 0, 0, 0x5A, 0x00, 0xFF, 0xFF, 0xFF, 0, 0, 3,
			       0xFF, 0xFF, 0xFF, 0xFF};

	zelos_someip_core_tick(&sip, now + 10); /* the first multicast offer */
	sd_in(e, 0);
	zelos_someip_core_tick(&sip, now + 11);
	zassert_equal(sd_to, PEER);
	zassert_equal(sd_out[24], 0x01);
	/* Its own session counter: the first unicast message is 1. */
	zassert_equal(sys_get_be16(&sd_out[10]), 1);
}

ZTEST(someip, test_unicast_sessions_per_peer)
{
	const uint8_t e[16] = {0x00, 0, 0, 0, 0x5A, 0x00, 0xFF, 0xFF, 0xFF, 0, 0, 3,
			       0xFF, 0xFF, 0xFF, 0xFF};

	/* Multicast offers do not count towards a peer's (PRS_SOMEIPSD_00256, 00631). */
	zelos_someip_core_tick(&sip, now + 10);
	zelos_someip_core_tick(&sip, now + 110);
	/* Each peer from 1, Reboot set; its own counter goes on. */
	for (uint32_t peer = PEER; peer < PEER + 2; peer++) {
		for (uint16_t want = 1; want <= 2; want++) {
			uint8_t m[16 + 8 + 16 + 4] = {0xFF, 0xFF, 0x81, 0x00, 0, 0, 0, 36};

			sys_put_be32(16, &m[20]);
			memcpy(&m[24], e, 16);
			zelos_someip_core_sd(&sip, peer, 30490, m, sizeof(m), now + 120);
			zelos_someip_core_tick(&sip, now + 120);
			zassert_equal(sd_to, peer);
			zassert_equal(sys_get_be16(&sd_out[10]), want);
			zassert_equal(sd_out[16] & 0x80, 0x80);
		}
	}
}

ZTEST_SUITE(someip, NULL, NULL, before, NULL, NULL);
