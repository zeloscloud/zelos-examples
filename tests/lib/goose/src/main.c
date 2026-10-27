/*
 * The GOOSE core driven by hand: values in, frames out, time injected. Fields
 * found by walking the BER PDU (IEC 61850-8-1 Annex A).
 */

#include <zelos/goose.h>

#include <string.h>

#include <zephyr/ztest.h>

#define STRIP(...) __VA_ARGS__
/* The core's time is in us. */
#define MS(ms) ((int64_t)(ms) * 1000)

static const uint8_t mac[6] = {0x02, 0x80, 0xE1, 0xAA, 0xAF, 0x4D};

static const struct zelos_goose_cb on_change = {
	.appid = 0x0001,
	.dst = {0x01, 0x0C, 0xCD, 0x01, 0x00, 0x01},
	.gocb_ref = "IED/LLN0$GO$gcb",
	.dat_set = "IED/LLN0$ds",
	.go_id = "id",
	.count = 1,
};

static const struct zelos_goose_cb periodic = {
	.appid = 0x0002,
	.dst = {0x01, 0x0C, 0xCD, 0x01, 0x00, 0x02},
	.gocb_ref = "IED/LLN0$GO$gcb",
	.dat_set = "IED/LLN0$ds",
	.go_id = "id",
	.period_ms = 100,
	.count = 1,
};

static uint8_t f[ZELOS_GOOSE_FRAME_MAX];
static size_t n;

/* Header size of the TLV at p, its content length in *len. */
static size_t tl(const uint8_t *p, size_t *len)
{
	if (p[1] < 0x80) {
		*len = p[1];
		return 2;
	}
	if (p[1] == 0x81) {
		*len = p[2];
		return 3;
	}
	*len = (size_t)p[2] << 8 | p[3];
	return 4;
}

/* The content of the PDU's field `tag`, its length in *len. */
static const uint8_t *field(uint8_t tag, size_t *len)
{
	const uint8_t *p = &f[22];
	const uint8_t *end;
	size_t pdu_len;

	zassert_equal(p[0], 0x61);
	p += tl(p, &pdu_len);
	end = p + pdu_len;
	zassert_equal_ptr(end, &f[n], "PDU ends with the frame");
	while (p < end) {
		const size_t h = tl(p, len);

		if (p[0] == tag) {
			return p + h;
		}
		p += h + *len;
	}
	zassert_unreachable("no field 0x%02x", tag);
	return NULL;
}

static int64_t integer(uint8_t tag)
{
	size_t len;
	const uint8_t *v = field(tag, &len);
	int64_t x = (int8_t)v[0];

	for (size_t i = 1; i < len; i++) {
		x = x * 256 + v[i];
	}
	return x;
}

#define TAL() integer(0x81)
#define ST_NUM() integer(0x85)
#define SQ_NUM() integer(0x86)

/* A one-member data set's frame ends with that member's encoding. */
#define ENCODES(value, ...)                                                                        \
	do {                                                                                       \
		const uint8_t want_[] = {__VA_ARGS__};                                             \
		struct zelos_goose_pub pub_ = {.cb = &on_change};                                  \
		pub_.values[0] = (struct zelos_goose_value)STRIP value;                            \
		zelos_goose_update(&pub_, 0, 1, 0);                                                \
		n = zelos_goose_poll(&pub_, 0, mac, f, sizeof(f));                                 \
		zassert_true(n > sizeof(want_));                                                   \
		zassert_mem_equal(&f[n - sizeof(want_)], want_, sizeof(want_));                    \
	} while (0)

ZTEST(goose, test_float32)
{
	ENCODES(({.type = ZELOS_GOOSE_FLOAT32, .f32 = 13.8f}), 0x87, 0x05, 0x08, 0x41, 0x5C, 0xCC,
		0xCD);
	ENCODES(({.type = ZELOS_GOOSE_FLOAT32, .f32 = -1.5f}), 0x87, 0x05, 0x08, 0xBF, 0xC0, 0x00,
		0x00);
}

ZTEST(goose, test_dbpos_and_boolean)
{
	ENCODES(({.type = ZELOS_GOOSE_DBPOS, .dbpos = ZELOS_GOOSE_ON}), 0x84, 0x02, 0x06, 0x80);
	ENCODES(({.type = ZELOS_GOOSE_DBPOS, .dbpos = ZELOS_GOOSE_OFF}), 0x84, 0x02, 0x06, 0x40);
	ENCODES(({.type = ZELOS_GOOSE_DBPOS, .dbpos = ZELOS_GOOSE_INTERMEDIATE}), 0x84, 0x02, 0x06,
		0x00);
	ENCODES(({.type = ZELOS_GOOSE_BOOLEAN, .boolean = true}), 0x83, 0x01, 0x01);
}

ZTEST(goose, test_int32)
{
	ENCODES(({.type = ZELOS_GOOSE_INT32, .i32 = -1}), 0x85, 0x01, 0xFF);
	ENCODES(({.type = ZELOS_GOOSE_INT32, .i32 = -129}), 0x85, 0x02, 0xFF, 0x7F);
	ENCODES(({.type = ZELOS_GOOSE_INT32, .i32 = INT32_MIN}), 0x85, 0x04, 0x80, 0x00, 0x00,
		0x00);
	ENCODES(({.type = ZELOS_GOOSE_INT32, .i32 = 128}), 0x85, 0x02, 0x00, 0x80);
	ENCODES(({.type = ZELOS_GOOSE_INT32, .i32 = 0}), 0x85, 0x01, 0x00);
}

ZTEST(goose, test_uint32)
{
	ENCODES(({.type = ZELOS_GOOSE_UINT32, .u32 = 127}), 0x86, 0x01, 0x7F);
	ENCODES(({.type = ZELOS_GOOSE_UINT32, .u32 = 0x80}), 0x86, 0x02, 0x00, 0x80);
	ENCODES(({.type = ZELOS_GOOSE_UINT32, .u32 = UINT32_MAX}), 0x86, 0x05, 0x00, 0xFF, 0xFF,
		0xFF, 0xFF);
}

ZTEST(goose, test_frame)
{
	struct zelos_goose_pub pub = {.cb = &on_change, .simulation = true};
	size_t len;

	pub.values[0] = (struct zelos_goose_value){.type = ZELOS_GOOSE_BOOLEAN};
	zelos_goose_update(&pub, 0, 7, MS(1500));
	n = zelos_goose_poll(&pub, MS(1500), mac, f, sizeof(f));

	zassert_mem_equal(f, on_change.dst, 6);
	zassert_mem_equal(&f[6], mac, 6);
	/* EtherType, APPID, Length from the APPID to the end, both reserved words 0. */
	zassert_mem_equal(&f[12], ((uint8_t[]){0x88, 0xB8, 0x00, 0x01}), 4);
	zassert_equal((size_t)(f[16] << 8 | f[17]), n - 14);
	zassert_mem_equal(&f[18], ((uint8_t[]){0, 0, 0, 0}), 4);

	zassert_mem_equal(field(0x80, &len), "IED/LLN0$GO$gcb", 15);
	zassert_equal(len, 15);
	zassert_mem_equal(field(0x82, &len), "IED/LLN0$ds", 11);
	zassert_mem_equal(field(0x83, &len), "id", 2);
	/* 1.5 s: seconds, half of 2^24, clock not synchronized. */
	zassert_mem_equal(field(0x84, &len), ((uint8_t[]){0, 0, 0, 1, 0x80, 0, 0, 0x3F}), 8);
	zassert_equal(len, 8);
	zassert_equal(*field(0x87, &len), 0x01, "simulation");
	zassert_equal(integer(0x88), 7, "confRev");
	zassert_equal(*field(0x89, &len), 0x00, "ndsCom");
	zassert_equal(integer(0x8A), 1, "numDatSetEntries");
	zassert_equal(ST_NUM(), 1);
	zassert_equal(SQ_NUM(), 0);
	zassert_equal(f[n - 5], 0xAB, "allData last");

	/* One octet short: no frame, the dry run zelos_goose_start() relies on. */
	zelos_goose_update(&pub, 1, 7, MS(1500));
	zassert_equal(zelos_goose_poll(&pub, MS(1500), mac, f, n - 1), 0);
}

ZTEST(goose, test_retransmission_and_tal)
{
	static const int64_t delta[] = {4, 8, 16, 32, 64, 128, 256, 512, 1000, 1000};
	struct zelos_goose_pub pub = {.cb = &on_change};
	int64_t now = MS(5000);

	zelos_goose_update(&pub, 0, 1, now);
	for (size_t i = 0; i < ARRAY_SIZE(delta); i++) {
		n = zelos_goose_poll(&pub, now, mac, f, sizeof(f));
		zassert_true(n > 0, "frame %zu", i);
		zassert_equal(SQ_NUM(), (int64_t)i);
		zassert_equal(ST_NUM(), 1);
		zassert_equal(TAL(), 2 * delta[i], "frame %zu", i);
		zassert_equal(zelos_goose_interval_ms(i), delta[i]);
		zassert_equal(zelos_goose_poll(&pub, now + MS(delta[i]) - 1, mac, f, sizeof(f)), 0);
		now += MS(delta[i]);
	}

	/* Unchanged: nothing before the next. Changed: at once, stNum + 1, sqNum 0, a burst. */
	now -= MS(500);
	zelos_goose_update(&pub, 0, 1, now);
	zassert_equal(zelos_goose_poll(&pub, now, mac, f, sizeof(f)), 0);
	zelos_goose_update(&pub, 1, 1, now);
	n = zelos_goose_poll(&pub, now, mac, f, sizeof(f));
	zassert_equal(ST_NUM(), 2);
	zassert_equal(SQ_NUM(), 0);
	zassert_equal(TAL(), 8);
	zassert_equal(pub.t_us, now);
	zassert_equal(pub.next_us, now + MS(4));
	/* A new confRev is a new state too. */
	zelos_goose_update(&pub, 1, 2, now + 1);
	zassert_equal(pub.st_num, 3);
	zassert_equal(pub.next_us, now + 1);
}

ZTEST(goose, test_periodic)
{
	struct zelos_goose_pub pub = {.cb = &periodic};

	zelos_goose_update(&pub, 0, 1, 0);
	for (int64_t now = 0; now < MS(300); now += MS(100)) {
		n = zelos_goose_poll(&pub, now, mac, f, sizeof(f));
		zassert_equal(SQ_NUM(), now / MS(100));
		zassert_equal(TAL(), 200);
		zassert_equal(zelos_goose_poll(&pub, now + MS(100) - 1, mac, f, sizeof(f)), 0);
	}
	/* A change keeps the period: stNum + 1 and sqNum 0 on the next frame, t of the change. */
	zelos_goose_update(&pub, 2, 1, MS(250));
	zassert_equal(zelos_goose_poll(&pub, MS(250), mac, f, sizeof(f)), 0);
	n = zelos_goose_poll(&pub, MS(300), mac, f, sizeof(f));
	zassert_equal(ST_NUM(), 2);
	zassert_equal(SQ_NUM(), 0);
	zassert_equal(TAL(), 200);
	zassert_equal(pub.t_us, MS(250));
}

ZTEST(goose, test_wrap_to_one)
{
	struct zelos_goose_pub pub = {.cb = &periodic};

	zassert_equal(zelos_goose_next(0), 1);
	zassert_equal(zelos_goose_next(UINT32_MAX - 1), UINT32_MAX);
	zassert_equal(zelos_goose_next(UINT32_MAX), 1);

	zelos_goose_update(&pub, 0, 1, 0);
	pub.sq_num = UINT32_MAX;
	n = zelos_goose_poll(&pub, 0, mac, f, sizeof(f));
	zassert_equal(SQ_NUM(), UINT32_MAX);
	n = zelos_goose_poll(&pub, MS(100), mac, f, sizeof(f));
	zassert_equal(SQ_NUM(), 1);

	pub.st_num = UINT32_MAX;
	zelos_goose_update(&pub, 1, 1, MS(150));
	zassert_equal(pub.st_num, 1);
	zassert_equal(pub.sq_num, 0);
}

ZTEST_SUITE(goose, NULL, NULL, NULL, NULL, NULL);
