/*
 * The XCP core driven by hand: CROs in, responses and DTOs out.
 */

#include <zelos/xcp.h>

#include <string.h>

#include <zephyr/ztest.h>

#define ERR_CMD_UNKNOWN   0x20
#define ERR_ACCESS_DENIED 0x24

/* A readable measurement, a writable parameter, and a secret nobody registered. */
static uint8_t meas[4] = {0x11, 0x22, 0x33, 0x44};
static uint32_t param = 0xAABBCCDD;
static uint32_t secret = 0x5EC7E7;

static const struct zelos_xcp_region regions[] = {
	{meas, sizeof(meas), false},
	{&param, sizeof(param), true},
};
static const struct zelos_xcp_event_channel events[] = {{"cycle", 50}};
static const struct zelos_xcp_config config = {
	.regions = regions,
	.region_count = ARRAY_SIZE(regions),
	.events = events,
	.event_count = ARRAY_SIZE(events),
	.id = "test",
};

static struct zelos_xcp_core xcp;

#define ADDR(p) ((uint32_t)(uintptr_t)(p))
#define LE32(a) (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24)

/* Send one CRO and return its single response. */
#define CMD(res, ...)                                                                              \
	do {                                                                                       \
		const uint8_t req_[] = {__VA_ARGS__};                                              \
		zelos_xcp_core_on_frame(&xcp, req_, sizeof(req_));                                 \
		zassert_true(zelos_xcp_core_poll(&xcp, (res)), "no response");                    \
		zassert_false(zelos_xcp_core_poll(&xcp, &(struct zelos_xcp_frame){0}),            \
			      "more than one response");                                           \
	} while (0)

static void assert_err(const struct zelos_xcp_frame *f, uint8_t code)
{
	zassert_equal(f->len, 2);
	zassert_equal(f->data[0], 0xFE);
	zassert_equal(f->data[1], code, "error 0x%02x", f->data[1]);
}

static void assert_ok(const struct zelos_xcp_frame *f)
{
	zassert_equal(f->data[0], 0xFF, "error 0x%02x", f->data[1]);
}

static void before(void *fixture)
{
	struct zelos_xcp_frame f;

	ARG_UNUSED(fixture);
	param = 0xAABBCCDD;
	zelos_xcp_core_init(&xcp, &config);
	CMD(&f, 0xFF, 0x00);
}

ZTEST(xcp, test_connect)
{
	const uint8_t expected[] = {0xFF, 0x05, 0x80, 8, 8, 0, 1, 1};
	struct zelos_xcp_frame f;

	/* Disconnected, the slave ignores everything but CONNECT. */
	zelos_xcp_core_init(&xcp, &config);
	zelos_xcp_core_on_frame(&xcp, (const uint8_t[]){0xFD}, 1);
	zassert_false(zelos_xcp_core_poll(&xcp, &f));

	CMD(&f, 0xFF, 0x00);
	zassert_equal(f.len, sizeof(expected));
	zassert_mem_equal(f.data, expected, sizeof(expected));

	CMD(&f, 0xC0); /* not implemented */
	assert_err(&f, ERR_CMD_UNKNOWN);
}

ZTEST(xcp, test_short_upload)
{
	struct zelos_xcp_frame f;

	CMD(&f, 0xF4, 2, 0, 0, LE32(ADDR(&meas[1])));
	assert_ok(&f);
	zassert_equal(f.len, 3);
	zassert_equal(f.data[1], 0x22);
	zassert_equal(f.data[2], 0x33);

	/* Straddling the end of a region is as denied as missing it. */
	CMD(&f, 0xF4, 2, 0, 0, LE32(ADDR(&meas[3])));
	assert_err(&f, ERR_ACCESS_DENIED);
	CMD(&f, 0xF4, 4, 0, 0, LE32(ADDR(&secret)));
	assert_err(&f, ERR_ACCESS_DENIED);
}

ZTEST(xcp, test_download_allowlist)
{
	struct zelos_xcp_frame f;

	CMD(&f, 0xF6, 0, 0, 0, LE32(ADDR(&param)));
	assert_ok(&f);
	CMD(&f, 0xF0, 4, LE32(0x01020304));
	assert_ok(&f);
	zassert_equal(param, 0x01020304);

	/* Readable is not writable. */
	CMD(&f, 0xF6, 0, 0, 0, LE32(ADDR(meas)));
	CMD(&f, 0xF0, 1, 0x99);
	assert_err(&f, ERR_ACCESS_DENIED);
	zassert_equal(meas[0], 0x11);

	CMD(&f, 0xF6, 0, 0, 0, LE32(ADDR(&secret)));
	CMD(&f, 0xF0, 4, LE32(0));
	assert_err(&f, ERR_ACCESS_DENIED);
	zassert_equal(secret, 0x5EC7E7);
}

ZTEST(xcp, test_daq_round_trip)
{
	struct zelos_xcp_frame f;

	CMD(&f, 0xD6); /* FREE_DAQ */
	assert_ok(&f);
	CMD(&f, 0xD5, 0, 1, 0); /* ALLOC_DAQ: one list */
	assert_ok(&f);
	CMD(&f, 0xD4, 0, 0, 0, 1); /* ALLOC_ODT: list 0, one ODT */
	assert_ok(&f);
	CMD(&f, 0xD3, 0, 0, 0, 0, 2); /* ALLOC_ODT_ENTRY: two entries */
	assert_ok(&f);
	CMD(&f, 0xE2, 0, 0, 0, 0, 0); /* SET_DAQ_PTR 0/0/0 */
	assert_ok(&f);

	/* An entry outside the regions is refused when written, not when sampled. */
	CMD(&f, 0xE1, 0xFF, 4, 0, LE32(ADDR(&secret)));
	assert_err(&f, ERR_ACCESS_DENIED);

	CMD(&f, 0xE1, 0xFF, 2, 0, LE32(ADDR(&meas[2])));
	assert_ok(&f);
	CMD(&f, 0xE1, 0xFF, 4, 0, LE32(ADDR(&param)));
	assert_ok(&f);
	CMD(&f, 0xE0, 0x00, 0, 0, 0, 0, 1, 0); /* SET_DAQ_LIST_MODE: event 0, prescaler 1 */
	assert_ok(&f);
	CMD(&f, 0xDE, 1, 0, 0); /* START_STOP_DAQ_LIST: start */
	assert_ok(&f);
	zassert_equal(f.data[1], 0, "first PID");

	zelos_xcp_core_event(&xcp, 0);
	zassert_true(zelos_xcp_core_poll(&xcp, &f));
	const uint8_t dto[] = {0x00, 0x33, 0x44, 0xDD, 0xCC, 0xBB, 0xAA};
	zassert_equal(f.len, sizeof(dto));
	zassert_mem_equal(f.data, dto, sizeof(dto));
	zassert_false(zelos_xcp_core_poll(&xcp, &f));

	CMD(&f, 0xDD, 0); /* START_STOP_SYNCH: stop all */
	zelos_xcp_core_event(&xcp, 0);
	zassert_false(zelos_xcp_core_poll(&xcp, &f));
}

ZTEST_SUITE(xcp, NULL, NULL, before, NULL, NULL);
