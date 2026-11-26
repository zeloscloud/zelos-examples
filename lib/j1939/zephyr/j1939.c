/*
 * J1939 on Zephyr: CAN frames in, the core on its own thread, frames out.
 */

#include <zelos/can.h>
#include <zelos/j1939.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_j1939, LOG_LEVEL_INF);

/* PF byte and the two data-page bits: every frame of one PGN, any address. */
#define PF_MASK 0x03FF0000U

/*
 * A poll sends at most a claim, an Acknowledgment, a BAM packet, an RTS/CTS
 * frame and one frame per message, so this many messages bound what it can
 * queue.
 */
#define MAX_MSGS 48

/* K_MSGQ_DEFINE cannot be static, so its name carries the library's. */
K_MSGQ_DEFINE(zelos_j1939_rx_msgq, sizeof(struct can_frame), 16, 4);
static K_MUTEX_DEFINE(lock);
static K_THREAD_STACK_DEFINE(stack, 1024);
static struct k_thread thread;

/* Frames a poll produced, queued under the lock and sent after it. */
static struct zelos_j1939_frame out[4 + MAX_MSGS];
static size_t out_len;

static void queue_frame(const struct zelos_j1939_frame *frame, void *user)
{
	ARG_UNUSED(user);
	__ASSERT_NO_MSG(out_len < ARRAY_SIZE(out));
	out[out_len++] = *frame;
}

static void run(void *p1, void *p2, void *p3)
{
	struct zelos_j1939 *j = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct can_frame frame;
		bool got = k_msgq_get(&zelos_j1939_rx_msgq, &frame,
				      K_MSEC(CONFIG_ZELOS_J1939_TICK_MS)) == 0;
		uint32_t now = k_uptime_get_32();

		k_mutex_lock(&lock, K_FOREVER);
		out_len = 0;
		/* Filters match remote frames too; they carry no J1939 data. */
		if (got && (frame.flags & CAN_FRAME_RTR) == 0U) {
			struct zelos_j1939_frame f = {
				.id = frame.id,
				.len = MIN(can_dlc_to_bytes(frame.dlc), 8),
			};

			memcpy(f.data, frame.data, f.len);
			zelos_j1939_on_frame(j, &f, now);
		}
		zelos_j1939_poll(j, now);
		k_mutex_unlock(&lock);

		/* Outside the lock: a slow bus must not block zelos_j1939_set(). */
		for (size_t i = 0; i < out_len; i++) {
			(void)zelos_can_send_ext(out[i].id, out[i].data, out[i].len);
		}
	}
}

int zelos_j1939_start(struct zelos_j1939 *j)
{
	static const uint32_t pgns[] = {ZELOS_J1939_PGN_REQUEST, ZELOS_J1939_PGN_ADDRESS_CLAIMED,
					ZELOS_J1939_PGN_TP_CM};
	int err;

	if (j->n_msgs > MAX_MSGS) {
		LOG_ERR("more than %d messages", MAX_MSGS);
		return -EINVAL;
	}

	j->send = queue_frame;
	(void)zelos_j1939_init(j, k_uptime_get_32());

	for (size_t i = 0; i < ARRAY_SIZE(pgns) + j->n_rx_pgns; i++) {
		uint32_t pgn = i < ARRAY_SIZE(pgns) ? pgns[i] : j->rx_pgns[i - ARRAY_SIZE(pgns)];

		err = zelos_can_subscribe_ext(pgn << 8, PF_MASK, &zelos_j1939_rx_msgq);
		if (err < 0) {
			LOG_ERR("subscribe to PGN %u: %d", pgn, err);
			return err;
		}
	}

	k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, j, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&thread, "j1939");

	LOG_INF("claiming 0x%02x", j->preferred);

	return 0;
}

void zelos_j1939_set(struct zelos_j1939_msg *msg, const uint8_t *data, uint16_t len)
{
	__ASSERT_NO_MSG(len <= ZELOS_J1939_MAX_LEN);

	k_mutex_lock(&lock, K_FOREVER);
	memcpy(msg->data, data, len);
	msg->len = len;
	k_mutex_unlock(&lock);
}

void zelos_j1939_send_to(struct zelos_j1939_msg *msg, uint8_t da)
{
	k_mutex_lock(&lock, K_FOREVER);
	msg->requested = true;
	msg->requester = da;
	k_mutex_unlock(&lock);
}

bool zelos_j1939_pending(struct zelos_j1939 *j, const struct zelos_j1939_msg *msg)
{
	bool pending;

	k_mutex_lock(&lock, K_FOREVER);
	pending = msg->requested || (j->bam.active && j->bam.pgn == msg->pgn) ||
		  (j->rts.state != ZELOS_J1939_RTS_IDLE && j->rts.pgn == msg->pgn);
	k_mutex_unlock(&lock);

	return pending;
}

void zelos_j1939_quiet(struct zelos_j1939 *j, bool quiet)
{
	k_mutex_lock(&lock, K_FOREVER);
	j->quiet = quiet;
	k_mutex_unlock(&lock);
}

void zelos_j1939_ack(struct zelos_j1939 *j, uint8_t to, uint32_t pgn)
{
	k_mutex_lock(&lock, K_FOREVER);
	j->ack_pending = true;
	j->ack_control = ZELOS_J1939_ACK;
	j->ack_to = to;
	j->ack_pgn = pgn;
	k_mutex_unlock(&lock);
}
