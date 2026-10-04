/*
 * The XCP core driven by hand: CROs in, responses and DTOs out.
 */

#include <zelos/xcp.h>

#include <string.h>

#include <zephyr/ztest.h>

#define ERR_DAQ_ACTIVE      0x11
#define ERR_CMD_UNKNOWN     0x20
#define ERR_WRITE_PROTECTED 0x23
#define ERR_ACCESS_DENIED   0x24
#define ERR_MODE_NOT_VALID  0x27
#define ERR_DAQ_CONFIG      0x2A

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
	.epk = "build 42",
};

static struct zelos_xcp_core xcp;

#define ADDR(p) ((uint32_t)(uintptr_t)(p))
#define LE32(a) (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24)

/* The DAQ clock the tests run at. */
#define NOW 0x12345678

/* Send one CRO and return its single response. */
#define CMD(res, ...)                                                                              \
	do {                                                                                       \
		const uint8_t req_[] = {__VA_ARGS__};                                              \
		zelos_xcp_core_on_frame(&xcp, req_, sizeof(req_), NOW);                            \
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
	zelos_xcp_core_on_frame(&xcp, (const uint8_t[]){0xFD}, 1, NOW);
	zassert_false(zelos_xcp_core_poll(&xcp, &f));

	CMD(&f, 0xFF, 0x00);
	zassert_equal(f.len, sizeof(expected));
	zassert_mem_equal(f.data, expected, sizeof(expected));

	CMD(&f, 0xC0); /* not implemented */
	assert_err(&f, ERR_CMD_UNKNOWN);
	CMD(&f, 0xED, 4, 0, 0, LE32(ADDR(&param))); /* SHORT_DOWNLOAD: no room in 8 bytes */
	assert_err(&f, ERR_CMD_UNKNOWN);
}

ZTEST(xcp, test_epk)
{
	struct zelos_xcp_frame f;

	CMD(&f, 0xFA, 5); /* GET_ID: EPK, uploaded from the MTA */
	assert_ok(&f);
	zassert_equal(f.data[1], 0, "mode");
	zassert_equal(f.data[4], strlen(config.epk));
	CMD(&f, 0xF5, 7);
	zassert_equal(f.len, 8);
	zassert_mem_equal(&f.data[1], "build 4", 7);

	CMD(&f, 0xFA, 3); /* URL: not available */
	assert_ok(&f);
	zassert_equal(f.data[4], 0);
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
	assert_err(&f, ERR_WRITE_PROTECTED);
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

	zelos_xcp_core_event(&xcp, 0, NOW);
	zassert_true(zelos_xcp_core_poll(&xcp, &f));
	const uint8_t dto[] = {0x00, 0x33, 0x44, 0xDD, 0xCC, 0xBB, 0xAA};
	zassert_equal(f.len, sizeof(dto));
	zassert_mem_equal(f.data, dto, sizeof(dto));
	zassert_false(zelos_xcp_core_poll(&xcp, &f));

	CMD(&f, 0xDD, 0); /* START_STOP_SYNCH: stop all */
	zelos_xcp_core_event(&xcp, 0, NOW);
	zassert_false(zelos_xcp_core_poll(&xcp, &f));
}

/* One list on event 0 holding one ODT with `size` bytes of meas. */
static void one_list(uint8_t size)
{
	struct zelos_xcp_frame f;

	CMD(&f, 0xD6);
	CMD(&f, 0xD5, 0, 1, 0);
	CMD(&f, 0xD4, 0, 0, 0, 1);
	CMD(&f, 0xD3, 0, 0, 0, 0, 1);
	CMD(&f, 0xE2, 0, 0, 0, 0, 0);
	CMD(&f, 0xE1, 0xFF, size, 0, LE32(ADDR(meas)));
	assert_ok(&f);
}

ZTEST(xcp, test_daq_timestamp)
{
	struct zelos_xcp_frame f;

	/* 4 data bytes and a 4-byte timestamp do not fit one CAN frame. */
	one_list(4);
	CMD(&f, 0xE0, 0x10, 0, 0, 0, 0, 1, 0);
	assert_err(&f, ERR_DAQ_CONFIG);

	one_list(2);
	CMD(&f, 0xE0, 0x10, 0, 0, 0, 0, 1, 0); /* timestamp on */
	assert_ok(&f);
	CMD(&f, 0xDE, 1, 0, 0);
	assert_ok(&f);

	/* The list's mode reads back: running, timestamped, event 0, prescaler 1. */
	CMD(&f, 0xDF, 0, 0, 0);
	const uint8_t mode[] = {0xFF, 0x50, 0, 0, 0, 0, 1, 0};
	zassert_mem_equal(f.data, mode, sizeof(mode));

	/* The timestamp follows the PID in ODT 0, from the same clock as GET_DAQ_CLOCK. */
	zelos_xcp_core_event(&xcp, 0, NOW);
	zassert_true(zelos_xcp_core_poll(&xcp, &f));
	const uint8_t dto[] = {0x00, LE32(NOW), 0x11, 0x22};
	zassert_equal(f.len, sizeof(dto));
	zassert_mem_equal(f.data, dto, sizeof(dto));
	CMD(&f, 0xDC);
	const uint8_t clock[] = {0xFF, 0, 0, 0, LE32(NOW)};
	zassert_mem_equal(f.data, clock, sizeof(clock));
}

ZTEST(xcp, test_daq_frozen_while_running)
{
	struct zelos_xcp_frame f;

	one_list(1);
	CMD(&f, 0xDE, 1, 0, 0);
	assert_ok(&f);

	/* Every command that changes the configuration, refused; the list keeps sampling. */
	CMD(&f, 0xD5, 0, 1, 0);
	assert_err(&f, ERR_DAQ_ACTIVE);
	CMD(&f, 0xD4, 0, 0, 0, 1);
	assert_err(&f, ERR_DAQ_ACTIVE);
	CMD(&f, 0xD3, 0, 0, 0, 0, 1);
	assert_err(&f, ERR_DAQ_ACTIVE);
	CMD(&f, 0xE1, 0xFF, 1, 0, LE32(ADDR(meas)));
	assert_err(&f, ERR_DAQ_ACTIVE);
	CMD(&f, 0xE0, 0x10, 0, 0, 0, 0, 1, 0);
	assert_err(&f, ERR_DAQ_ACTIVE);
	zelos_xcp_core_event(&xcp, 0, NOW);
	zassert_true(zelos_xcp_core_poll(&xcp, &f));
	zassert_equal(f.len, 2, "untimestamped sample");

	/* Stopped, it is configurable again. */
	CMD(&f, 0xDE, 0, 0, 0);
	CMD(&f, 0xE0, 0x10, 0, 0, 0, 0, 1, 0);
	assert_ok(&f);
}

ZTEST(xcp, test_mode_not_valid)
{
	struct zelos_xcp_frame f;

	one_list(1);
	CMD(&f, 0xDE, 3, 0, 0); /* START_STOP_DAQ_LIST: no mode 3 */
	assert_err(&f, ERR_MODE_NOT_VALID);
	CMD(&f, 0xDD, 3); /* START_STOP_SYNCH: no mode 3 */
	assert_err(&f, ERR_MODE_NOT_VALID);
	CMD(&f, 0xE0, 0x02, 0, 0, 0, 0, 1, 0); /* SET_DAQ_LIST_MODE: STIM */
	assert_err(&f, ERR_MODE_NOT_VALID);
	CMD(&f, 0xE0, 0x20, 0, 0, 0, 0, 1, 0); /* PID_OFF */
	assert_err(&f, ERR_MODE_NOT_VALID);
}

ZTEST(xcp, test_daq_info)
{
	struct zelos_xcp_frame f;

	/* Dynamic, prescaler, timestamps, overload by event. */
	CMD(&f, 0xDA);
	zassert_equal(f.data[1], 0x93);
	/* Timestamps: 4 bytes, 1 us per tick, not fixed. */
	CMD(&f, 0xD9);
	zassert_equal(f.data[5], 0x34);
	zassert_equal(f.data[6], 1);
	zassert_equal(f.data[7], 0);
	/* DAQ event, consistency per event, as the A2L says. */
	CMD(&f, 0xD7, 0, 0, 0);
	zassert_equal(f.data[1], 0x84);
}

ZTEST(xcp, test_daq_overload)
{
	struct zelos_xcp_frame f;
	int samples = 0;

	one_list(1);
	CMD(&f, 0xE0, 0x00, 0, 0, 0, 0, 1, 0);
	CMD(&f, 0xDE, 1, 0, 0);

	/* Nothing drains the queue: the samples that do not fit are dropped whole. */
	for (int i = 0; i < 2 * CONFIG_ZELOS_XCP_ODTS; i++) {
		zelos_xcp_core_event(&xcp, 0, NOW);
	}
	while (zelos_xcp_core_poll(&xcp, &f) && f.data[0] < 0xFC) {
		samples++;
	}
	/* One slot stays free for a response; the overload follows the samples. */
	zassert_equal(samples, CONFIG_ZELOS_XCP_ODTS);
	const uint8_t ev[] = {0xFD, 0x06};
	zassert_equal(f.len, sizeof(ev));
	zassert_mem_equal(f.data, ev, sizeof(ev));
	zassert_false(zelos_xcp_core_poll(&xcp, &f));
}

ZTEST_SUITE(xcp, NULL, NULL, before, NULL, NULL);
