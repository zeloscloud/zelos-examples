/*
 * The UDS core driven by hand: requests in, responses out, time injected.
 */

#include <zelos/uds.h>

#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#define SEED 0x11223344U
#define KEY1 (SEED ^ ZELOS_UDS_KEY_XOR_EXTENDED)
#define KEY11 (SEED ^ ZELOS_UDS_KEY_XOR_PROGRAMMING)
#define DELAY_MS 10000
#define STRIP(...) __VA_ARGS__
#define BE32(v) (uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)
#define WIN 0x00, 0x10, 0x00, 0x00 /* CONFIG_ZELOS_UDS_DOWNLOAD_ADDRESS */
#define P2 0x00, 0x32, 0x01, 0xF4

static uint8_t cal[2] = {0x01, 0x02};
static uint8_t live = 7;
static bool adjusted;
static uint8_t ro_window[4] = {1, 2, 3, 4};
static uint8_t rw_window[4];

static void read_cal(uint8_t *out)
{
	memcpy(out, cal, sizeof(cal));
}

static uint8_t write_cal(const uint8_t *in)
{
	if (in[0] == 0xFF) {
		return 0x31;
	}
	memcpy(cal, in, sizeof(cal));
	return 0;
}

static void read_live(uint8_t *out)
{
	out[0] = live;
}

static void read_io(uint8_t *out)
{
	out[0] = adjusted;
}

static uint8_t control_io(uint8_t param, const uint8_t *in)
{
	adjusted = param == ZELOS_UDS_IO_ADJUST && in[0] != 0;
	return 0;
}

static uint8_t self_test(uint8_t *result)
{
	result[0] = 0x00;
	return 1;
}

static const struct zelos_uds_did dids[] = {
	{0x0200, 2, read_cal, write_cal},
	{0x0101, 1, read_live, NULL},
};
static const struct zelos_uds_io ios[] = {{0x0300, 1, read_io, control_io}};
static const struct zelos_uds_routine routines[] = {
	{0x0201, ZELOS_UDS_SESSION_EXTENDED, 2000, self_test},
};
static const struct zelos_uds_memory memories[] = {
	{0x00010000, sizeof(ro_window), ro_window, false},
	{0x00020000, sizeof(rw_window), rw_window, true},
};
/* WriteMemoryByAddress stores through the application's hook. */
static int writes;

static void count_write(void *dst, const void *src, size_t len)
{
	writes++;
	memcpy(dst, src, len);
}

static const uint32_t dtcs[] = {0x1A0098, 0xC11187};
static const uint16_t snapshot_dids[] = {0x0101};
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
	.write = count_write,
};

static struct zelos_uds_core uds;
static struct zelos_uds_dtc_memory memory;
static uint8_t rsp[CONFIG_ZELOS_UDS_BUF_SIZE];
static size_t rsp_len;
static int64_t now;

static uint32_t fixed_seed(void)
{
	return SEED;
}

#define REQ(functional, ...)                                                                       \
	do {                                                                                       \
		const uint8_t req_[] = {__VA_ARGS__};                                              \
		rsp_len = zelos_uds_core_request(&uds, req_, sizeof(req_), functional, now, rsp);  \
	} while (0)

/* Physical request; the response must be exactly the bytes given. */
#define EXPECT(rsp_bytes, ...)                                                                     \
	do {                                                                                       \
		const uint8_t want_[] = {STRIP rsp_bytes};                                         \
		REQ(false, __VA_ARGS__);                                                           \
		zassert_equal(rsp_len, sizeof(want_), "length %zu", rsp_len);                      \
		zassert_mem_equal(rsp, want_, sizeof(want_));                                      \
	} while (0)

#define NRC(sid, nrc, ...) EXPECT((0x7F, sid, nrc), sid, ##__VA_ARGS__)
#define SILENT(...)                                                                                \
	do {                                                                                       \
		REQ(true, __VA_ARGS__);                                                            \
		zassert_equal(rsp_len, 0, "answered 0x%02x", rsp[2]);                              \
	} while (0)

static void boot(void)
{
	zassert_ok(zelos_uds_core_init(&uds, &config, fixed_seed, &memory, now));
}

/* Past the power-up delay, in a session, with its level unlocked. */
static void unlocked(uint8_t session)
{
	now += DELAY_MS;
	EXPECT((0x50, 0x03, P2), 0x10, 0x03);
	if (session == 0x02) {
		EXPECT((0x50, 0x02, P2), 0x10, 0x02);
		EXPECT((0x67, 0x11, BE32(SEED)), 0x27, 0x11);
		EXPECT((0x67, 0x12), 0x27, 0x12, BE32(KEY11));
	} else {
		EXPECT((0x67, 0x01, BE32(SEED)), 0x27, 0x01);
		EXPECT((0x67, 0x02), 0x27, 0x02, BE32(KEY1));
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	now = 1000;
	adjusted = false;
	memset(&memory, 0xA5, sizeof(memory)); /* power-on garbage */
	boot();
}

ZTEST(uds, test_nrc_priority)
{
	NRC(0x29, 0x11, 0x01);                  /* Tier B: not supported */
	NRC(0x85, 0x7F, 0x02);                  /* before its sub-function */
	NRC(0x10, 0x13);                        /* minimum length */
	NRC(0x10, 0x12, 0x04, 0x00);            /* sub-function before exact length */
	NRC(0x10, 0x22, 0x02);                  /* programming only from extended */
	EXPECT((0x50, 0x03, P2), 0x10, 0x03);
	NRC(0x27, 0x12, 0x05);
	NRC(0x27, 0x7E, 0x11);                  /* level 0x11 is programming's */
	NRC(0x27, 0x37, 0x01);                  /* power-up delay */
	NRC(0x2E, 0x31, 0x12, 0x34, 0x00);      /* identifier before security */
	NRC(0x2E, 0x33, 0x02, 0x00, 0x00);      /* security before exact length */
	NRC(0x2F, 0x31, 0x03, 0x00, 0x07);      /* parameter before security */
	NRC(0x28, 0x31, 0x00, 0x02);            /* communication type before security */
	NRC(0x28, 0x33, 0x00, 0x01);
	NRC(0x3D, 0x31, 0x44, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x09); /* read-only */
	NRC(0x3D, 0x33, 0x14, 0x00, 0x02, 0x00, 0x00, 0x01, 0x09);

	/* Functional: silent for 0x11, 0x12, 0x31, 0x7E, 0x7F only. */
	SILENT(0x29, 0x01);
	SILENT(0x27, 0x05);
	SILENT(0x27, 0x11);
	SILENT(0x34, 0x00, 0x11, 0x00, 0x01);
	SILENT(0x22, 0x12, 0x34);
	REQ(true, 0x3E, 0x00, 0x00);
	zassert_mem_equal(rsp, ((uint8_t[]){0x7F, 0x3E, 0x13}), 3);
	REQ(true, 0x27, 0x01);
	zassert_mem_equal(rsp, ((uint8_t[]){0x7F, 0x27, 0x37}), 3);
	SILENT(0x3E, 0x80);
}

ZTEST(uds, test_session_side_effects)
{
	unlocked(0x03);
	EXPECT((0x68, 0x01), 0x28, 0x01, 0x01);
	EXPECT((0xC5, 0x02), 0x85, 0x02);
	EXPECT((0x6F, 0x03, 0x00, 0x03, 0x01), 0x2F, 0x03, 0x00, 0x03, 0x01);
	zassert_true(adjusted);
	zassert_equal(uds.communication, 0x01);

	/* Extended again: relocked, output returned, communication kept. */
	EXPECT((0x50, 0x03, P2), 0x10, 0x03);
	zassert_false(adjusted);
	zassert_equal(uds.communication, 0x01);
	NRC(0x2E, 0x33, 0x02, 0x00, 0x00, 0x01);

	/* S3: default, communication and DTC setting back on. */
	now += 4999;
	zassert_equal(zelos_uds_core_tick(&uds, now, rsp), 0);
	zassert_equal(uds.session, 0x03);
	now += 1;
	zelos_uds_core_tick(&uds, now, rsp);
	EXPECT((0x62, 0xF1, 0x86, 0x01), 0x22, 0xF1, 0x86);
	zassert_equal(uds.communication, 0x00);
	zassert_true(uds.dtc_setting);
}

ZTEST(uds, test_security_levels)
{
	EXPECT((0x50, 0x03, P2), 0x10, 0x03);
	now += DELAY_MS - 1;
	NRC(0x27, 0x37, 0x01);
	now += 1;
	NRC(0x27, 0x24, 0x02, BE32(KEY1));
	for (int i = 0; i < 2; i++) {
		EXPECT((0x67, 0x01, BE32(SEED)), 0x27, 0x01);
		NRC(0x27, 0x35, 0x02, BE32(KEY11)); /* the other level's key */
	}
	EXPECT((0x67, 0x01, BE32(SEED)), 0x27, 0x01);
	NRC(0x27, 0x36, 0x02, BE32(KEY1 ^ 1));
	NRC(0x27, 0x37, 0x01);

	/* Level 0x11 keeps its own count and delay. */
	EXPECT((0x50, 0x02, P2), 0x10, 0x02);
	EXPECT((0x67, 0x11, BE32(SEED)), 0x27, 0x11);
	EXPECT((0x67, 0x12), 0x27, 0x12, BE32(KEY11));
	EXPECT((0x67, 0x11, BE32(0)), 0x27, 0x11);

	EXPECT((0x50, 0x03, P2), 0x10, 0x03);
	now += DELAY_MS - 1;
	NRC(0x27, 0x37, 0x01);
	now += 1;
	EXPECT((0x67, 0x01, BE32(SEED)), 0x27, 0x01);
	EXPECT((0x67, 0x02), 0x27, 0x02, BE32(KEY1));
}

static uint8_t status(size_t i)
{
	return memory.dtc[i].status;
}

static void reset(void)
{
	zelos_uds_core_persist(&uds);
	boot();
}

ZTEST(uds, test_dtc_status_over_operation_cycles)
{
	/* Power-on garbage: cleared, nothing tested. */
	zassert_equal(status(0), 0x50);
	EXPECT((0x59, 0x01, 0xFF, 0x01, 0x00, 0x00), 0x19, 0x01, 0x09);

	live = 42;
	zelos_uds_core_dtc(&uds, 0, true);
	zassert_equal(status(0), 0xAF); /* TF TFTOC PDTC CDTC TFSLC WIR */
	live = 43;
	zelos_uds_core_dtc(&uds, 0, true);
	zelos_uds_core_dtc(&uds, 0, false);
	zassert_equal(status(0), 0x2E);
	zelos_uds_core_dtc(&uds, 1, false);
	zassert_equal(status(1), 0x00);

	reset();
	zassert_equal(status(0), 0x6C); /* a new cycle: TNCTOC, not TFTOC */
	zelos_uds_core_dtc(&uds, 0, true);
	EXPECT((0x59, 0x06, 0x1A, 0x00, 0x98, 0xAF, 0x01, 0x02, 0x02, 0x00), 0x19, 0x06, 0x1A,
	       0x00, 0x98, 0xFF);
	/* The snapshot is the first failure's. */
	EXPECT((0x59, 0x03, 0x1A, 0x00, 0x98, 0x01), 0x19, 0x03);
	EXPECT((0x59, 0x04, 0x1A, 0x00, 0x98, 0xAF, 0x01, 0x01, 0x01, 0x01, 42), 0x19, 0x04, 0x1A,
	       0x00, 0x98, 0x01);
	zelos_uds_core_dtc(&uds, 0, false);

	/*
	 * Passing from here: pending clears after the first whole cycle passed,
	 * confirmed after three.
	 */
	static const uint8_t passed[] = {0x2C, 0x28, 0x28, 0x20};

	for (int cycle = 0; cycle < ARRAY_SIZE(passed); cycle++) {
		reset();
		zelos_uds_core_dtc(&uds, 0, false);
		zassert_equal(status(0), passed[cycle], "cycle %d: 0x%02x", cycle, status(0));
	}

	/* Not reset in order: the memory starts cleared. */
	boot();
	zassert_equal(status(0), 0x50);

	/* DTC setting off: results ignored, clear still clears. */
	unlocked(0x03);
	EXPECT((0xC5, 0x02), 0x85, 0x02);
	zelos_uds_core_dtc(&uds, 1, true);
	zassert_equal(status(1), 0x50);
	EXPECT((0xC5, 0x01), 0x85, 0x01);
	zelos_uds_core_dtc(&uds, 1, true);
	EXPECT((0x59, 0x02, 0xFF, 0xC1, 0x11, 0x87, 0xAF), 0x19, 0x02, 0x01);
	NRC(0x14, 0x31, 0x00, 0x00, 0x01);
	EXPECT((0x54), 0x14, 0xC1, 0x11, 0x87);
	zassert_equal(status(1), 0x50);
	NRC(0x19, 0x12, 0x82, 0xFF);
}

ZTEST(uds, test_transfer_sequence)
{
	static const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	uint32_t crc = crc32_ieee(data, sizeof(data));

	unlocked(0x02);
	NRC(0x34, 0x31, 0x00, 0x44, WIN, 0x00, 0x00, 0x01, 0x01); /* past the window */
	NRC(0x34, 0x70, 0x00, 0x44, WIN, 0x00, 0x00, 0x00, 0x0A); /* not erased */
	NRC(0x31, 0x24, 0x01, 0xFF, 0x01, BE32(crc));              /* nothing downloaded */

	NRC(0x31, 0x78, 0x01, 0xFF, 0x00);
	NRC(0x22, 0x21, 0xF1, 0x86);
	SILENT(0x29, 0x01); /* functional: unsupported */
	REQ(true, 0x10, 0x83); /* functional: suppressPosRsp hides positive responses only */
	zassert_equal(rsp_len, 3);
	zassert_mem_equal(rsp, ((uint8_t[]){0x7F, 0x10, 0x21}), 3);
	now += 999;
	zassert_equal(zelos_uds_core_tick(&uds, now, rsp), 0);
	now += 1;
	zassert_equal(zelos_uds_core_tick(&uds, now, rsp), 5);
	zassert_mem_equal(rsp, ((uint8_t[]){0x71, 0x01, 0xFF, 0x00, 0x00}), 5);

	EXPECT((0x74, 0x20, CONFIG_ZELOS_UDS_BUF_SIZE >> 8, CONFIG_ZELOS_UDS_BUF_SIZE & 0xFF),
	       0x34, 0x00, 0x44, WIN, 0x00, 0x00, 0x00, 0x0A);
	NRC(0x34, 0x22, 0x00, 0x44, WIN, 0x00, 0x00, 0x00, 0x0A);
	NRC(0x34, 0x31, 0x11, 0x44, WIN, 0x00, 0x00, 0x00, 0x0A); /* format before 0x22 */
	NRC(0x36, 0x73, 0x02, 1);
	EXPECT((0x76, 0x01), 0x36, 0x01, 1, 2, 3, 4);
	EXPECT((0x76, 0x01), 0x36, 0x01, 9, 9, 9, 9); /* a repeat: not written again */
	NRC(0x37, 0x24);
	NRC(0x36, 0x71, 0x02, 5, 6, 7, 8, 9, 10, 11);
	EXPECT((0x76, 0x02), 0x36, 0x02, 5, 6, 7, 8, 9, 10);
	EXPECT((0x77), 0x37);
	EXPECT((0x71, 0x01, 0xFF, 0x01, 0x00, BE32(crc)), 0x31, 0x01, 0xFF, 0x01, BE32(crc));
	EXPECT((0x71, 0x01, 0xFF, 0x01, 0x01, BE32(crc)), 0x31, 0x01, 0xFF, 0x01, BE32(crc ^ 1));

	/* Written memory is no longer erased. */
	NRC(0x34, 0x70, 0x00, 0x44, WIN, 0x00, 0x00, 0x00, 0x0A);

	/* Upload reads it back, a repeat resent. */
	EXPECT((0x75, 0x20, CONFIG_ZELOS_UDS_BUF_SIZE >> 8, CONFIG_ZELOS_UDS_BUF_SIZE & 0xFF),
	       0x35, 0x00, 0x14, WIN, 0x0A);
	EXPECT((0x76, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 0x36, 0x01);
	EXPECT((0x76, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 0x36, 0x01);
	NRC(0x36, 0x24, 0x02);
	EXPECT((0x77), 0x37);

	/* A session change ends a transfer. */
	EXPECT((0x75, 0x20, CONFIG_ZELOS_UDS_BUF_SIZE >> 8, CONFIG_ZELOS_UDS_BUF_SIZE & 0xFF),
	       0x35, 0x00, 0x14, WIN, 0x0A);
	EXPECT((0x50, 0x02, P2), 0x10, 0x02);
	NRC(0x36, 0x24, 0x01);
}

ZTEST(uds, test_memory_by_address)
{
	EXPECT((0x63, 2, 3), 0x23, 0x14, 0x00, 0x01, 0x00, 0x01, 0x02);
	NRC(0x23, 0x31, 0x14, 0x00, 0x01, 0x00, 0x03, 0x02); /* straddles the end */
	NRC(0x23, 0x31, 0x22, 0x00, 0x01, 0x00, 0x01, 0x02); /* 2-byte address */
	NRC(0x23, 0x13, 0x44, 0x00, 0x01, 0x00, 0x00, 0x02);

	unlocked(0x03);
	writes = 0;
	EXPECT((0x7D, 0x24, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02), 0x3D, 0x24, 0x00, 0x02, 0x00,
	       0x01, 0x00, 0x02, 0xAB, 0xCD);
	zassert_mem_equal(&rw_window[1], ((uint8_t[]){0xAB, 0xCD}), 2);
	zassert_equal(writes, 1);
	NRC(0x3D, 0x13, 0x14, 0x00, 0x02, 0x00, 0x00, 0x02, 0xAB);
}

ZTEST(uds, test_pending_routine)
{
	unlocked(0x03);
	NRC(0x31, 0x24, 0x03, 0x02, 0x01);
	NRC(0x31, 0x24, 0x02, 0x02, 0x01);
	NRC(0x31, 0x31, 0x01, 0xFF, 0x00); /* programming's */
	/* Pending even with suppressPosRsp: the result always follows a 0x78. */
	NRC(0x31, 0x78, 0x81, 0x02, 0x01);
	SILENT(0x3E, 0x80);
	now += 1999;
	zassert_equal(zelos_uds_core_tick(&uds, now, rsp), 0);
	now += 1;
	zassert_equal(zelos_uds_core_tick(&uds, now, rsp), 5);
	zassert_mem_equal(rsp, ((uint8_t[]){0x71, 0x01, 0x02, 0x01, 0x00}), 5);
	EXPECT((0x71, 0x03, 0x02, 0x01, 0x00), 0x31, 0x03, 0x02, 0x01);
}

ZTEST_SUITE(uds, NULL, NULL, before, NULL, NULL);
