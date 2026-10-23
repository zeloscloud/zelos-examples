/*
 * The DoIP core driven by hand: bytes in, messages out, time injected. Byte
 * vectors are ISO 13400-2:2012's layouts, as the W4 simulator sends them.
 */

#include <zelos/doip.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define ECU 0x10, 0x01
#define TESTER 0x0E, 0x80
#define VIN "1ZLSDCDC0BENCH001"
#define HDR(type, len) 0x02, 0xFD, (type) >> 8, (type) & 0xFF, 0, 0, (len) >> 8, (len) & 0xFF
#define STRIP(...) __VA_ARGS__

static const uint8_t eid[6] = {0x02, 0x80, 0xE1, 0xAA, 0xAF, 0x4D};

static void vin(uint8_t out[ZELOS_DOIP_VIN_SIZE])
{
	memcpy(out, VIN, ZELOS_DOIP_VIN_SIZE);
}

static const struct zelos_doip_config config = {
	.logical_address = 0x1001,
	.functional_address = 0xE400,
	.vin = vin,
};

/* What the core did: per socket, the last message sent and whether it closed. */
static uint8_t sent[ZELOS_DOIP_SOCKETS][64];
static size_t sent_len[ZELOS_DOIP_SOCKETS];
static int sends[ZELOS_DOIP_SOCKETS];
static bool closed[ZELOS_DOIP_SOCKETS];
static uint16_t diagnosed_from;
static bool diagnosed_functional;

static bool fake_send(void *ctx, int s, const uint8_t *msg, size_t len)
{
	ARG_UNUSED(ctx);
	memcpy(sent[s], msg, MIN(len, sizeof(sent[s])));
	sent_len[s] = len;
	sends[s]++;
	return true;
}

static void fake_close(void *ctx, int s)
{
	ARG_UNUSED(ctx);
	closed[s] = true;
}

/* Echoes a positive response to any UDS request: SID + 0x40. */
static size_t fake_diagnose(void *ctx, uint16_t tester, const uint8_t *req, size_t len,
			    bool functional, uint8_t *rsp)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(len);
	diagnosed_from = tester;
	diagnosed_functional = functional;
	rsp[0] = req[0] + 0x40;
	return 1;
}

static const struct zelos_doip_ops ops = {fake_send, fake_close, fake_diagnose};
static struct zelos_doip_core doip;
static int64_t now;

#define FEED(s, ...)                                                                               \
	do {                                                                                       \
		const uint8_t in_[] = {__VA_ARGS__};                                               \
		zelos_doip_core_receive(&doip, s, in_, sizeof(in_), now);                          \
	} while (0)

#define SENT(s, ...)                                                                               \
	do {                                                                                       \
		const uint8_t want_[] = {__VA_ARGS__};                                             \
		zassert_equal(sent_len[s], sizeof(want_), "length %zu", sent_len[s]);              \
		zassert_mem_equal(sent[s], want_, sizeof(want_));                                  \
	} while (0)

#define BE16(v) (uint8_t)((v) >> 8), (uint8_t)(v)
#define ACTIVATE(s, source, type) FEED(s, HDR(0x0005, 7), BE16(source), type, 0, 0, 0, 0)
#define ACTIVATION(s, source, code) SENT(s, HDR(0x0006, 9), BE16(source), ECU, code, 0, 0, 0, 0)

/* A new connection, its record fresh. */
static int open_socket(void)
{
	int s = zelos_doip_core_accept(&doip, now);

	if (s >= 0) {
		sent_len[s] = 0;
		sends[s] = 0;
		closed[s] = false;
	}
	return s;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(sent_len, 0, sizeof(sent_len));
	memset(sends, 0, sizeof(sends));
	memset(closed, 0, sizeof(closed));
	now = 1000;
	zelos_doip_core_init(&doip, &config, eid, &ops, NULL);
}

ZTEST(doip, test_vehicle_identification)
{
	uint8_t out[ZELOS_DOIP_MESSAGE_MAX];
	bool delay;

	/* Plain, version 0xFF: the announcement's bytes, after a delay. */
	static const uint8_t plain[] = {0xFF, 0x00, 0x00, 0x01, 0, 0, 0, 0};
	size_t n = zelos_doip_core_udp(&doip, plain, sizeof(plain), out, &delay);

	zassert_true(delay);
	zassert_equal(n, 8 + 32);
	zassert_mem_equal(&out[8], VIN, 17);
	zassert_mem_equal(&out[25], ((uint8_t[]){ECU}), 2);
	zassert_mem_equal(&out[27], eid, 6);
	zassert_mem_equal(&out[33], eid, 6);
	zassert_equal(out[39], 0x00);

	/* By EID: ours answers, another gets nothing. */
	uint8_t by_eid[] = {HDR(0x0002, 6), 0x02, 0x80, 0xE1, 0xAA, 0xAF, 0x4D};

	zassert_equal(zelos_doip_core_udp(&doip, by_eid, sizeof(by_eid), out, &delay), 40);
	by_eid[13] = 0;
	zassert_equal(zelos_doip_core_udp(&doip, by_eid, sizeof(by_eid), out, &delay), 0);

	/* Entity status: node, 2 sockets, none open, MDS. */
	static const uint8_t status[] = {HDR(0x4001, 0)};

	zassert_equal(zelos_doip_core_udp(&doip, status, sizeof(status), out, &delay), 15);
	zassert_false(delay);
	zassert_mem_equal(out, ((uint8_t[]){HDR(0x4002, 7), 0x01, 2, 0}), 11);
	zassert_equal(sys_get_be32(&out[11]), ZELOS_DOIP_MDS);

	/* NACKs: bad pattern, unknown type, wrong length. */
	static const uint8_t pattern[] = {0x02, 0x02, 0x00, 0x01, 0, 0, 0, 0};
	static const uint8_t unknown[] = {HDR(0x0042, 0)};
	static const uint8_t length[] = {HDR(0x0001, 1), 0};

	zassert_equal(zelos_doip_core_udp(&doip, pattern, sizeof(pattern), out, &delay), 9);
	zassert_mem_equal(out, ((uint8_t[]){HDR(0x0000, 1), 0x00}), 9);
	zelos_doip_core_udp(&doip, unknown, sizeof(unknown), out, &delay);
	zassert_equal(out[8], 0x01);
	zelos_doip_core_udp(&doip, length, sizeof(length), out, &delay);
	zassert_equal(out[8], 0x04);
}

ZTEST(doip, test_routing_activation)
{
	int a = open_socket();
	int b = open_socket();
	int c = open_socket();

	zassert_equal(open_socket(), -1);

	/* Unknown source, unsupported type: refused and closed. */
	ACTIVATE(a, 0x1234, 0x00);
	ACTIVATION(a, 0x1234, 0x00);
	zassert_true(closed[a]);
	ACTIVATE(b, 0x0E80, 0x01);
	ACTIVATION(b, 0x0E80, 0x06);
	zassert_true(closed[b]);

	/* Activated; again with the same address. */
	a = open_socket();
	ACTIVATE(a, 0x0E80, 0x00);
	ACTIVATION(a, 0x0E80, 0x10);
	ACTIVATE(a, 0x0E80, 0xE0);
	ACTIVATION(a, 0x0E80, 0x10);
	zassert_equal(zelos_doip_core_testers(&doip), 1);

	/* The same address on another socket: an alive check decides. Answered: 0x03. */
	ACTIVATE(c, 0x0E80, 0x00);
	SENT(a, HDR(0x0007, 0));
	zassert_equal(sends[c], 0);
	FEED(a, HDR(0x0008, 2), TESTER);
	ACTIVATION(c, 0x0E80, 0x03);
	zassert_true(closed[c]);
	zassert_false(closed[a]);

	/* Unanswered within 500 ms: the old socket goes, the new one takes over. */
	c = open_socket();
	ACTIVATE(c, 0x0E80, 0x00);
	now += 499;
	zelos_doip_core_tick(&doip, now);
	zassert_false(closed[a]);
	now += 1;
	zelos_doip_core_tick(&doip, now);
	zassert_true(closed[a]);
	ACTIVATION(c, 0x0E80, 0x10);

	/* A second tester, then a third while both answer: 0x01. */
	a = open_socket();
	ACTIVATE(a, 0x0E81, 0x00);
	ACTIVATION(a, 0x0E81, 0x10);
	b = open_socket();
	ACTIVATE(b, 0x0E82, 0x00);
	FEED(a, HDR(0x0008, 2), 0x0E, 0x81);
	FEED(c, HDR(0x0008, 2), TESTER);
	ACTIVATION(b, 0x0E82, 0x01);
	zassert_true(closed[b]);
	zassert_equal(zelos_doip_core_testers(&doip), 2);

	/* Another address on an activated socket: 0x02 and closed. */
	ACTIVATE(a, 0x0E83, 0x00);
	ACTIVATION(a, 0x0E83, 0x02);
	zassert_true(closed[a]);
}

ZTEST(doip, test_diagnostic_messages)
{
	int s = open_socket();

	/* Before activation: NACK 0x02 and closed. */
	FEED(s, HDR(0x8001, 5), TESTER, ECU, 0x22);
	SENT(s, HDR(0x8003, 5), ECU, TESTER, 0x02);
	zassert_true(closed[s]);

	s = open_socket();
	ACTIVATE(s, 0x0E80, 0x00);
	/* Physical, split across reads: ACK, then the response from the ECU. */
	FEED(s, HDR(0x8001, 6), TESTER);
	FEED(s, ECU, 0x10, 0x03);
	zassert_equal(sends[s], 3);
	SENT(s, HDR(0x8001, 5), ECU, TESTER, 0x50);
	zassert_equal(diagnosed_from, 0x0E80);
	zassert_false(diagnosed_functional);
	/* Functional: acknowledged from the functional address, answered from the ECU's. */
	FEED(s, HDR(0x8001, 5), TESTER, 0xE4, 0x00, 0x3E);
	zassert_true(diagnosed_functional);
	SENT(s, HDR(0x8001, 5), ECU, TESTER, 0x7E);
	/* Unknown target: NACK 0x03, kept. */
	FEED(s, HDR(0x8001, 5), TESTER, 0x20, 0x00, 0x3E);
	SENT(s, HDR(0x8003, 5), 0x20, 0x00, TESTER, 0x03);
	zassert_false(closed[s]);
	/* Larger than MDS: NACK 0x04, its payload dropped, the stream still in step. */
	const uint32_t big = ZELOS_DOIP_MDS + 1;
	uint8_t header[] = {0x02, 0xFD, 0x80, 0x01, BE16(big >> 16), BE16(big), TESTER, ECU};
	static uint8_t rest[ZELOS_DOIP_MDS + 1 - 4];

	zelos_doip_core_receive(&doip, s, header, sizeof(header), now);
	SENT(s, HDR(0x8003, 5), ECU, TESTER, 0x04);
	zelos_doip_core_receive(&doip, s, rest, sizeof(rest), now);
	FEED(s, HDR(0x8001, 5), TESTER, ECU, 0x3E);
	SENT(s, HDR(0x8001, 5), ECU, TESTER, 0x7E);
	/* Bad pattern: NACK 0x00 and closed. */
	FEED(s, 0x02, 0x02, 0x80, 0x01, 0, 0, 0, 5);
	SENT(s, HDR(0x0000, 1), 0x00);
	zassert_true(closed[s]);
}

ZTEST(doip, test_inactivity)
{
	int s = open_socket();

	/* No activation within 2 s. */
	now += 1999;
	zelos_doip_core_tick(&doip, now);
	zassert_false(closed[s]);
	now += 1;
	zelos_doip_core_tick(&doip, now);
	zassert_true(closed[s]);

	/* Activated: 300 s of silence, restarted by traffic. */
	s = open_socket();
	ACTIVATE(s, 0x0E80, 0x00);
	now += 299999;
	FEED(s, HDR(0x8001, 5), TESTER, ECU, 0x3E);
	now += 299999;
	zelos_doip_core_tick(&doip, now);
	zassert_false(closed[s]);
	now += 1;
	zelos_doip_core_tick(&doip, now);
	zassert_true(closed[s]);
}

ZTEST(doip, test_entities_are_not_answered)
{
	/* Another entity's NACK, announcement or status: nothing, or two could bounce. */
	static const uint8_t nack[] = {HDR(0x0000, 1), 0x01};
	static const uint8_t vam[] = {HDR(0x0004, 0)};
	static const uint8_t status[] = {HDR(0x4002, 0)};
	uint8_t out[ZELOS_DOIP_MESSAGE_MAX];
	bool delay;

	zassert_equal(zelos_doip_core_udp(&doip, nack, sizeof(nack), out, &delay), 0);
	zassert_equal(zelos_doip_core_udp(&doip, vam, sizeof(vam), out, &delay), 0);
	zassert_equal(zelos_doip_core_udp(&doip, status, sizeof(status), out, &delay), 0);
}

ZTEST(doip, test_activation_after_the_checked_socket_closes)
{
	int a = open_socket();
	int b;

	ACTIVATE(a, 0x0E80, 0x00);
	b = open_socket();
	ACTIVATE(b, 0x0E80, 0x00);
	SENT(a, HDR(0x0007, 0));
	/* The old tester's socket goes (RST) before it answers: the new one takes over. */
	zelos_doip_core_close(&doip, a);
	zelos_doip_core_tick(&doip, now);
	ACTIVATION(b, 0x0E80, 0x10);
}

ZTEST_SUITE(doip, NULL, NULL, before, NULL, NULL);
