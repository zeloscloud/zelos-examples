#include <zelos/can.h>

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(zelos_can, LOG_LEVEL_INF);

#define DEPTH CONFIG_ZELOS_CAN_TX_QUEUE_DEPTH
#define MAX_AGE_MS CONFIG_ZELOS_CAN_TX_MAX_AGE_MS

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/*
 * Pending frames, sorted by arbitration key, highest first: the next to send
 * is the last. Insertion is a scan and a move of at most DEPTH entries.
 */
static struct {
	uint32_t key;
	uint32_t queued_ms;
	struct can_frame frame;
	struct zelos_can_done *done;
} queue[DEPTH];
static size_t queued;
static uint32_t peak;
static struct k_spinlock queue_lock;
/* Free slots, which senders wait on, and queued frames, which the thread does. */
static K_SEM_DEFINE(queue_room, DEPTH, DEPTH);
static K_SEM_DEFINE(queue_frames, 0, DEPTH);

static atomic_t sent;
static atomic_t overflow;
static atomic_t deferred;
static atomic_t tx_error;

/*
 * The frame's place in bus arbitration, as one number: lower wins. The
 * arbitration field is the 11-bit base identifier, then RTR (standard) or SRR
 * (extended, always recessive), then IDE, then the extended frame's other 18
 * bits and its RTR. So a standard frame beats an extended one with the same
 * first 11 bits, and a data frame beats a remote one.
 */
static uint32_t arbitration_key(const struct can_frame *frame)
{
	const uint32_t rtr = (frame->flags & CAN_FRAME_RTR) != 0U ? 1U : 0U;

	if ((frame->flags & CAN_FRAME_IDE) != 0U) {
		return ((frame->id >> 18) << 21) | BIT(20) | BIT(19) |
		       ((frame->id & 0x3FFFFU) << 1) | rtr;
	}

	return (frame->id << 21) | (rtr << 20);
}

static int queue_frame(const struct can_frame *frame, k_timeout_t timeout, atomic_t *refusals,
		       struct zelos_can_done *done)
{
	const uint32_t key = arbitration_key(frame);
	k_spinlock_key_t lock;
	size_t i;

	if (k_sem_take(&queue_room, timeout) != 0) {
		atomic_inc(refusals);
		return -EAGAIN;
	}

	lock = k_spin_lock(&queue_lock);
	/* Ahead of every equal key, which then leaves first: FIFO within an id. */
	for (i = 0; i < queued && queue[i].key > key; i++) {
	}
	memmove(&queue[i + 1], &queue[i], (queued - i) * sizeof(queue[0]));
	queue[i].key = key;
	queue[i].queued_ms = k_uptime_get_32();
	queue[i].frame = *frame;
	queue[i].done = done;
	queued++;
	peak = MAX(peak, queued);
	k_spin_unlock(&queue_lock, lock);

	k_sem_give(&queue_frames);

	return 0;
}

int zelos_can_queue(const struct can_frame *frame, k_timeout_t timeout)
{
	return queue_frame(frame, timeout, &overflow, NULL);
}

int zelos_can_try_queue(const struct can_frame *frame)
{
	return queue_frame(frame, K_NO_WAIT, &deferred, NULL);
}

void zelos_can_dropped(void)
{
	atomic_inc(&overflow);
}

static void tx_done(const struct device *dev, int error, void *user_data)
{
	struct zelos_can_done *done = user_data;

	ARG_UNUSED(dev);

	atomic_inc(error == 0 ? &sent : &tx_error);
	if (done != NULL) {
		done->fn(done, error);
	}
}

static void tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct can_frame frame;
		struct zelos_can_done *done;
		k_spinlock_key_t lock;
		uint32_t age;
		int err;

		(void)k_sem_take(&queue_frames, K_FOREVER);
		lock = k_spin_lock(&queue_lock);
		frame = queue[--queued].frame;
		done = queue[queued].done;
		age = k_uptime_get_32() - queue[queued].queued_ms;
		k_spin_unlock(&queue_lock, lock);
		k_sem_give(&queue_room);

		/*
		 * Waits on the driver's own count of free buffers, up to the
		 * frame's remaining age; a stale frame is not sent. A frame queued
		 * meanwhile with a lower id goes after this one, so priority is
		 * inverted by at most one frame; among the frames already in its
		 * buffers the controller sends the lowest id first.
		 */
		err = age < MAX_AGE_MS ? can_send(can_dev, &frame, K_MSEC(MAX_AGE_MS - age), tx_done, done)
				       : -EAGAIN;
		if (err != 0) {
			atomic_inc(&tx_error);
			if (done != NULL) {
				done->fn(done, err);
			}
			/* Every frame fails under bus-off: one line per interval, then a skip count. */
			LOG_WRN_RATELIMIT("send of 0x%03x: %d", frame.id, err);
		}
	}
}

K_THREAD_DEFINE(zelos_can_tx, 1024, tx_thread, NULL, NULL, NULL, CONFIG_ZELOS_CAN_TX_PRIORITY, 0,
		SYS_FOREVER_MS);

int zelos_can_init(void)
{
	int err;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	/* Explicitly, rather than the devicetree's default. See Kconfig. */
	err = can_set_bitrate(can_dev, CONFIG_ZELOS_CAN_BITRATE);
	if (err != 0) {
		LOG_ERR("can_set_bitrate: %d", err);
		return err;
	}

	err = can_start(can_dev);
	if (err != 0) {
		LOG_ERR("can_start: %d", err);
		return err;
	}

	k_thread_start(zelos_can_tx);
	LOG_INF("CAN up at %d bit/s", CONFIG_ZELOS_CAN_BITRATE);

	return 0;
}

static int send_frame(uint32_t id, uint8_t flags, const uint8_t *data, uint8_t len,
		      struct zelos_can_done *done)
{
	struct can_frame frame = {.id = id, .flags = flags};
	int err;

	if (len > sizeof(frame.data)) {
		return -EINVAL;
	}

	frame.dlc = can_bytes_to_dlc(len);
	memcpy(frame.data, data, len);

	err = queue_frame(&frame, K_MSEC(10), &overflow, done);
	if (err != 0) {
		/* A full queue fails every caller alike: rate-limited as well. */
		LOG_WRN_RATELIMIT("send of 0x%03x: %d", id, err);
	}

	return err;
}

int zelos_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
	return send_frame(id, 0, data, len, NULL);
}

int zelos_can_send_done(uint32_t id, const uint8_t *data, uint8_t len, struct zelos_can_done *done)
{
	const int err = send_frame(id, 0, data, len, done);

	if (err != 0) {
		done->fn(done, err);
	}

	return err;
}

int zelos_can_send_ext(uint32_t id, const uint8_t *data, uint8_t len)
{
	return send_frame(id, CAN_FRAME_IDE, data, len, NULL);
}

int zelos_can_subscribe(uint32_t id, struct k_msgq *msgq)
{
	const struct can_filter filter = {
		.id = id,
		.mask = CAN_STD_ID_MASK,
	};

	/* The msgq variant runs in thread context; the callback variant is an ISR. */
	return can_add_rx_filter_msgq(can_dev, msgq, &filter);
}

int zelos_can_subscribe_ext(uint32_t id, uint32_t mask, struct k_msgq *msgq)
{
	const struct can_filter filter = {
		.id = id,
		.mask = mask,
		.flags = CAN_FILTER_IDE,
	};

	return can_add_rx_filter_msgq(can_dev, msgq, &filter);
}

struct zelos_can_stats zelos_can_stats(void)
{
	return (struct zelos_can_stats){
		.sent = (uint32_t)atomic_get(&sent),
		.overflow = (uint32_t)atomic_get(&overflow),
		.deferred = (uint32_t)atomic_get(&deferred),
		.tx_error = (uint32_t)atomic_get(&tx_error),
		.peak = peak,
#if defined(CONFIG_CAN_STATS)
		.rx_overrun = can_stats_get_rx_overruns(can_dev),
#endif
	};
}
