/*
 * The J1939 core driven by hand: frames in, time forward, frames out.
 */

#include <zelos/j1939.h>

#include <string.h>

#include <zephyr/ztest.h>

#define SA       0x80U
#define TESTER   0xF9U
#define PGN_ET1  65262U
#define PGN_DM1  65226U
#define TICK_MS  10U

#define ID(prio, pgn, sa) (((uint32_t)(prio) << 26) | ((uint32_t)(pgn) << 8) | (sa))
#define CLAIM_ID(sa)      ID(6, 0xEEFFU, sa)

#define FIXED     ZELOS_J1939_NAME(0, 0, 0, 0, 0, 0, 0, 0, 100)
#define ARBITRARY ZELOS_J1939_NAME(1, 0, 0, 0, 0, 0, 0, 0, 100)

static struct {
	struct zelos_j1939_frame frame;
	uint32_t at;
} sent[64];
static int n_sent;
static uint32_t now;

static uint8_t et1[8] = {0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t dm1[20];
static struct zelos_j1939_msg msgs[2];
static struct zelos_j1939 j;

static void capture(const struct zelos_j1939_frame *frame, void *user)
{
	zassert_true(n_sent < ARRAY_SIZE(sent));
	sent[n_sent].frame = *frame;
	sent[n_sent].at = now;
	n_sent++;
}

static void start_at(uint64_t name, uint32_t t0)
{
	for (size_t i = 0; i < sizeof(dm1); i++) {
		dm1[i] = (uint8_t)i;
	}
	msgs[0] = (struct zelos_j1939_msg){
		.pgn = PGN_ET1, .priority = 6, .period_ms = 1000, .len = 8, .data = et1};
	msgs[1] = (struct zelos_j1939_msg){
		.pgn = PGN_DM1, .priority = 6, .period_ms = 0, .len = sizeof(dm1), .data = dm1};
	j = (struct zelos_j1939){
		.name = name,
		.preferred = SA,
		.msgs = msgs,
		.n_msgs = ARRAY_SIZE(msgs),
		.send = capture,
	};
	n_sent = 0;
	now = t0;
	zassert_ok(zelos_j1939_init(&j, now));
	zelos_j1939_poll(&j, now);
}

static void start(uint64_t name)
{
	start_at(name, 0);
}

/* Advance to `until`, polling every tick as the Zephyr glue does. Wrap-safe. */
static void run_until(uint32_t until)
{
	while ((int32_t)(until - now) > 0) {
		now += TICK_MS;
		zelos_j1939_poll(&j, now);
	}
}

static void inject(uint32_t id, const uint8_t *data, uint8_t len)
{
	struct zelos_j1939_frame f = {.id = id, .len = len};

	memcpy(f.data, data, len);
	zelos_j1939_on_frame(&j, &f, now);
}

static void inject_claim(uint8_t sa, uint64_t name)
{
	uint8_t d[8];

	for (int i = 0; i < 8; i++) {
		d[i] = (uint8_t)(name >> (8 * i));
	}
	inject(CLAIM_ID(sa), d, sizeof(d));
}

static void inject_request(uint8_t da, uint32_t pgn)
{
	const uint8_t d[3] = {pgn & 0xFF, (pgn >> 8) & 0xFF, pgn >> 16};

	inject(ID(6, 0xEA00U | da, TESTER), d, sizeof(d));
}

/* Index of the first frame with this id sent at or after `from`, or -1. */
static int find(uint32_t id, int from)
{
	for (int i = from; i < n_sent; i++) {
		if (sent[i].frame.id == id) {
			return i;
		}
	}
	return -1;
}

ZTEST(j1939, test_claim_then_wait)
{
	start(FIXED);

	zassert_equal(n_sent, 1);
	zassert_equal(sent[0].frame.id, CLAIM_ID(SA));
	zassert_mem_equal(sent[0].frame.data, "\x64\0\0\0\0\0\0\0", 8);

	run_until(1000);
	/* Nothing but the claim for 250 ms, then the periodic message. */
	int et1_at = find(ID(6, PGN_ET1, SA), 0);

	zassert_equal(et1_at, 1);
	zassert_equal(sent[et1_at].at, ZELOS_J1939_CLAIM_WAIT_MS);
	zassert_equal(j.state, ZELOS_J1939_CLAIMED);
}

ZTEST(j1939, test_contention_won_by_lower_name)
{
	start(FIXED);
	run_until(100);
	inject_claim(SA, FIXED + 1);
	run_until(110);

	/* Defended: the claim again, same address. */
	zassert_equal(sent[n_sent - 1].frame.id, CLAIM_ID(SA));
	run_until(300);
	zassert_equal(j.state, ZELOS_J1939_CLAIMED);
	zassert_equal(j.address, SA);
}

ZTEST(j1939, test_contention_lost_fixed_address)
{
	start(FIXED);
	run_until(500);
	inject_claim(SA, FIXED - 1);
	int from = n_sent;

	run_until(3000);
	zassert_equal(j.state, ZELOS_J1939_CANNOT_CLAIM);
	zassert_equal(sent[from].frame.id, CLAIM_ID(ZELOS_J1939_ADDR_NULL));
	/* Cannot Claim Address, then silence. */
	zassert_equal(n_sent, from + 1);
}

ZTEST(j1939, test_contention_lost_moves_address)
{
	start(ARBITRARY);
	run_until(500);
	inject_claim(SA, ARBITRARY - 1);
	int from = n_sent;

	run_until(3000);
	zassert_equal(j.address, SA + 1);
	zassert_equal(sent[from].frame.id, CLAIM_ID(SA + 1));
	int et1_at = find(ID(6, PGN_ET1, SA + 1), from);

	zassert_true(et1_at > from);
	zassert_true(sent[et1_at].at - sent[from].at >= ZELOS_J1939_CLAIM_WAIT_MS);
}

ZTEST(j1939, test_requests)
{
	int from;

	start(FIXED);
	run_until(500);
	from = n_sent;

	inject_request(SA, PGN_ET1);
	inject_request(ZELOS_J1939_ADDR_GLOBAL, ZELOS_J1939_PGN_ADDRESS_CLAIMED);
	run_until(510);
	zassert_true(find(ID(6, PGN_ET1, SA), from) >= 0);
	zassert_true(find(CLAIM_ID(SA), from) >= 0);

	/* A PGN we lack: NACK to a request addressed to us, silence to a global one. */
	from = n_sent;
	inject_request(ZELOS_J1939_ADDR_GLOBAL, 0xFEE5U);
	run_until(520);
	zassert_equal(n_sent, from);
	inject_request(SA, 0xFEE5U);
	run_until(530);
	zassert_equal(n_sent, from + 1);
	zassert_equal(sent[from].frame.id, ID(6, 0xE8FFU, SA));
	zassert_mem_equal(sent[from].frame.data, "\x01\xFF\xFF\xFF\xF9\xE5\xFE\x00", 8);

	/* Requests to another address are not ours. */
	inject_request(SA + 1, PGN_ET1);
	run_until(540);
	zassert_equal(n_sent, from + 1);
}

/* DM1 went out by BAM: the announcement, then three packets at legal gaps. */
static void assert_bam(void)
{
	const uint8_t cm[8] = {32, sizeof(dm1), 0, 3, 0xFF, 0xCA, 0xFE, 0x00};
	int at;

	at = find(ID(7, 0xECFFU, SA), 0);
	zassert_true(at >= 0);
	zassert_mem_equal(sent[at].frame.data, cm, 8);

	for (uint8_t seq = 1; seq <= 3; seq++) {
		int prev = at;
		uint8_t expect[8];

		at = find(ID(7, 0xEBFFU, SA), prev + 1);
		zassert_true(at > prev);
		/* Every gap, CM to DT1 included, inside J1939-21's 50 to 200 ms. */
		zassert_between_inclusive(sent[at].at - sent[prev].at, 50, 200);

		memset(expect, 0xFF, sizeof(expect));
		expect[0] = seq;
		memcpy(&expect[1], &dm1[(seq - 1) * 7], MIN(7, sizeof(dm1) - (seq - 1) * 7));
		zassert_mem_equal(sent[at].frame.data, expect, 8);
	}
	zassert_equal(find(ID(7, 0xEBFFU, SA), at + 1), -1);
}

ZTEST(j1939, test_bam)
{
	start(FIXED);
	run_until(500);
	inject_request(ZELOS_J1939_ADDR_GLOBAL, PGN_DM1);
	run_until(1000);
	assert_bam();
}

/* The uptime counter wraps after 49.7 days; timing must not notice. */
ZTEST(j1939, test_wraparound)
{
	/* The claim wait spans the wrap. */
	start_at(FIXED, UINT32_MAX - 100U);
	run_until(1000);
	int et1_at = find(ID(6, PGN_ET1, SA), 0);

	zassert_equal(et1_at, 1);
	zassert_equal(sent[et1_at].at - sent[0].at, ZELOS_J1939_CLAIM_WAIT_MS);

	/* So does a BAM. */
	start_at(FIXED, UINT32_MAX - 300U);
	run_until(UINT32_MAX - 40U);
	zassert_equal(j.state, ZELOS_J1939_CLAIMED);
	inject_request(ZELOS_J1939_ADDR_GLOBAL, PGN_DM1);
	run_until(500);
	assert_bam();
}

/* RTS/CTS of DM1 to the tester: TP.CM and TP.DT carry the tester as DA. */
#define CM_TO_TESTER ID(7, 0xEC00U | TESTER, SA)
#define DT_TO_TESTER ID(7, 0xEB00U | TESTER, SA)

/* A TP.CM from the tester to us about DM1. */
static void inject_cm(uint8_t control, uint8_t b1, uint8_t b2)
{
	const uint8_t d[8] = {control, b1, b2, 0xFF, 0xFF, 0xCA, 0xFE, 0x00};

	inject(ID(7, 0xEC00U | SA, TESTER), d, sizeof(d));
}

/* Claimed, asked for DM1 by the tester alone, and the RTS sent. */
static int rts_started(void)
{
	const uint8_t rts[8] = {16, sizeof(dm1), 0, 3, 0xFF, 0xCA, 0xFE, 0x00};
	int at;

	start(FIXED);
	run_until(500);
	inject_request(SA, PGN_DM1);
	run_until(510);
	at = find(CM_TO_TESTER, 0);
	zassert_true(at >= 0);
	zassert_equal(sent[at].frame.len, 8);
	zassert_mem_equal(sent[at].frame.data, rts, 8);
	/* Nothing to everyone: no BAM. */
	zassert_equal(find(ID(7, 0xECFFU, SA), 0), -1);
	return at;
}

/* The DTs sent from `from` on, as sequence numbers, each checked against dm1. */
static int dts(int from, uint8_t *seqs)
{
	int n = 0;

	for (int i = find(DT_TO_TESTER, from); i >= 0; i = find(DT_TO_TESTER, i + 1)) {
		uint8_t seq = sent[i].frame.data[0];
		uint8_t expect[8];

		memset(expect, 0xFF, sizeof(expect));
		expect[0] = seq;
		memcpy(&expect[1], &dm1[(seq - 1) * 7], MIN(7, sizeof(dm1) - (seq - 1) * 7));
		zassert_equal(sent[i].frame.len, 8);
		zassert_mem_equal(sent[i].frame.data, expect, 8);
		seqs[n++] = seq;
	}
	return n;
}

ZTEST(j1939, test_rts_cts_windows)
{
	uint8_t seqs[8];
	int from = rts_started();

	/* No packet before the receiver's CTS. */
	run_until(1000);
	zassert_equal(dts(from, seqs), 0);

	inject_cm(17, 2, 1);
	run_until(1100);
	zassert_equal(dts(from, seqs), 2);
	zassert_mem_equal(seqs, "\x01\x02", 2);

	inject_cm(17, 2, 3);
	run_until(1200);
	zassert_equal(dts(from, seqs), 3);
	zassert_equal(seqs[2], 3);

	inject_cm(19, sizeof(dm1), 0);
	from = n_sent;
	run_until(4000);
	/* Done: no abort, no packet, a fresh request starts afresh. */
	zassert_equal(find(CM_TO_TESTER, from), -1);
	inject_request(SA, PGN_DM1);
	run_until(4010);
	zassert_equal(sent[find(CM_TO_TESTER, from)].frame.data[0], 16);
}

ZTEST(j1939, test_rts_cts_retransmit)
{
	uint8_t seqs[8];
	int from = rts_started();

	inject_cm(17, 3, 1);
	run_until(600);
	/* Packet 2 lost: the receiver asks for it again, then for 3. */
	inject_cm(17, 1, 2);
	run_until(700);
	inject_cm(17, 1, 3);
	run_until(800);
	zassert_equal(dts(from, seqs), 5);
	zassert_mem_equal(seqs, "\x01\x02\x03\x02\x03", 5);
}

ZTEST(j1939, test_rts_cts_receiver_abort)
{
	uint8_t seqs[8];
	int from = rts_started();

	inject_cm(255, 1, 0xFF);
	run_until(4000);
	/* Over: no packet, no abort back, the session is free. */
	zassert_equal(dts(from, seqs), 0);
	zassert_equal(find(CM_TO_TESTER, from + 1), -1);
	inject_request(SA, PGN_DM1);
	run_until(4010);
	zassert_equal(sent[find(CM_TO_TESTER, from + 1)].frame.data[0], 16);
}

/* The session's abort: reason 3, sent `wait` ms after `since`. */
static void assert_timeout_abort(int from, uint32_t since, uint32_t wait)
{
	const uint8_t abort[8] = {255, 3, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00};
	int at = find(CM_TO_TESTER, from);

	zassert_true(at >= 0);
	zassert_equal(sent[at].frame.len, 8);
	zassert_mem_equal(sent[at].frame.data, abort, 8);
	zassert_between_inclusive(sent[at].at - since, wait, wait + TICK_MS);
	zassert_equal(find(CM_TO_TESTER, at + 1), -1);
}

ZTEST(j1939, test_rts_cts_timeouts)
{
	uint8_t seqs[8];
	int from = rts_started();

	/* T3 after the RTS. */
	run_until(4000);
	assert_timeout_abort(from + 1, sent[from].at, ZELOS_J1939_T3_MS);

	/* T3 after a window's last packet. */
	from = rts_started();
	inject_cm(17, 1, 1);
	run_until(1000);
	zassert_equal(dts(from, seqs), 1);
	run_until(4000);
	assert_timeout_abort(from + 1, sent[find(DT_TO_TESTER, from)].at, ZELOS_J1939_T3_MS);

	/* T4 after a hold. */
	from = rts_started();
	run_until(1000);
	inject_cm(17, 0, 0xFF);
	run_until(4000);
	assert_timeout_abort(from + 1, 1000, ZELOS_J1939_T4_MS);
}

ZTEST_SUITE(j1939, NULL, NULL, NULL, NULL, NULL);
