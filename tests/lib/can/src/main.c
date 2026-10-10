/*
 * lib/can's transmit queue on the loopback controller: frames leave in bus
 * arbitration order, a full queue refuses and counts: overflow for a
 * sender that gives up, deferred for one that retries, and a stale frame is
 * dropped as a transmit error.
 */

#include <zelos/can.h>

#include <zephyr/ztest.h>

#define DEPTH CONFIG_ZELOS_CAN_TX_QUEUE_DEPTH
/* An extended identifier whose first 11 bits are standard 0x0C0's. */
#define EXT_0C0 ((0x0C0U << 18) | 0x1234U)

/* One queue behind every filter, so arrival order is transmission order. */
K_MSGQ_DEFINE(test_rx, sizeof(struct can_frame), DEPTH, 4);

static struct can_frame frame(uint32_t id, uint8_t flags, uint8_t tag)
{
	return (struct can_frame){.id = id, .flags = flags, .dlc = 1, .data = {tag}};
}

/* Queue them all before the transmit thread can take one. */
static void queue_all(const struct can_frame *frames, size_t n)
{
	k_sched_lock();
	for (size_t i = 0; i < n; i++) {
		zassert_ok(zelos_can_queue(&frames[i], K_NO_WAIT));
	}
	k_sched_unlock();
}

static void expect_tags(const uint8_t *tags, size_t n)
{
	struct can_frame got;

	for (size_t i = 0; i < n; i++) {
		zassert_ok(k_msgq_get(&test_rx, &got, K_SECONDS(1)), "frame %zu", i);
		zassert_equal(got.data[0], tags[i], "frame %zu: tag %u", i, got.data[0]);
	}
}

static void *setup(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

	zassert_ok(can_set_mode(dev, CAN_MODE_LOOPBACK));
	zassert_ok(zelos_can_init());
	zassert_true(zelos_can_subscribe(0x0C0, &test_rx) >= 0);
	zassert_true(zelos_can_subscribe(0x100, &test_rx) >= 0);
	zassert_true(zelos_can_subscribe(0x300, &test_rx) >= 0);
	zassert_true(zelos_can_subscribe_ext(EXT_0C0, CAN_EXT_ID_MASK, &test_rx) >= 0);

	return NULL;
}

ZTEST(zelos_can, test_bus_arbitration_order)
{
	const struct can_frame frames[] = {
		frame(0x300, 0, 1), frame(0x100, 0, 2), frame(EXT_0C0, CAN_FRAME_IDE, 3),
		frame(0x0C0, 0, 4), frame(0x100, 0, 5),
	};
	/* Lowest id first; standard before extended on one 11-bit prefix; FIFO within an id. */
	const uint8_t order[] = {4, 3, 2, 5, 1};

	queue_all(frames, ARRAY_SIZE(frames));
	expect_tags(order, ARRAY_SIZE(order));
}

ZTEST(zelos_can, test_full_queue_refuses_and_counts)
{
	const struct zelos_can_stats before = zelos_can_stats();
	struct can_frame frames[DEPTH];
	uint8_t tags[DEPTH];
	const struct can_frame extra = frame(0x0C0, 0, 0xFF);
	struct zelos_can_stats after;

	for (int i = 0; i < DEPTH; i++) {
		frames[i] = frame(0x300, 0, i);
		tags[i] = i;
	}

	k_sched_lock();
	queue_all(frames, DEPTH);
	zassert_equal(zelos_can_queue(&extra, K_NO_WAIT), -EAGAIN);
	zassert_equal(zelos_can_try_queue(&extra), -EAGAIN);
	k_sched_unlock();

	expect_tags(tags, DEPTH);
	after = zelos_can_stats();
	zassert_equal(after.overflow, before.overflow + 1);
	zassert_equal(after.deferred, before.deferred + 1);
	zassert_equal(after.sent, before.sent + DEPTH);
	zassert_equal(after.peak, DEPTH);
	zassert_equal(after.tx_error, before.tx_error);
}

ZTEST(zelos_can, test_stale_frames_dropped)
{
	const struct zelos_can_stats before = zelos_can_stats();
	const struct can_frame frames[] = {frame(0x100, 0, 1), frame(0x300, 0, 2)};
	const struct can_frame fresh = frame(0x0C0, 0, 3);
	const uint8_t tags[] = {3};
	struct can_frame got;
	struct zelos_can_stats after;

	k_sched_lock();
	queue_all(frames, ARRAY_SIZE(frames));
	k_busy_wait((CONFIG_ZELOS_CAN_TX_MAX_AGE_MS + 1) * USEC_PER_MSEC);
	zassert_ok(zelos_can_queue(&fresh, K_NO_WAIT));
	k_sched_unlock();

	expect_tags(tags, ARRAY_SIZE(tags));
	zassert_equal(k_msgq_get(&test_rx, &got, K_MSEC(100)), -EAGAIN, "stale frame 0x%03x sent", got.id);
	after = zelos_can_stats();
	zassert_equal(after.tx_error, before.tx_error + ARRAY_SIZE(frames));
	zassert_equal(after.sent, before.sent + 1);
}

ZTEST_SUITE(zelos_can, NULL, setup, NULL, NULL, NULL);
