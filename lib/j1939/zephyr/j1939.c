/*
 * J1939 on Zephyr: CAN frames in, the core on its own thread, frames out.
 */

#include <zelos/can.h>
#include <zelos/j1939.h>

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_j1939, LOG_LEVEL_INF);

/* PF byte and the two data-page bits: every frame of one PGN, any address. */
#define PF_MASK 0x03FF0000U

K_MSGQ_DEFINE(rx_msgq, sizeof(struct can_frame), 16, 4);
K_MUTEX_DEFINE(lock);
K_THREAD_STACK_DEFINE(stack, 1024);
static struct k_thread thread;

static void send_frame(const struct zelos_j1939_frame *frame, void *user)
{
	ARG_UNUSED(user);
	(void)zelos_can_send_ext(frame->id, frame->data, frame->len);
}

static void run(void *p1, void *p2, void *p3)
{
	struct zelos_j1939 *j = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct can_frame frame;
		bool got = k_msgq_get(&rx_msgq, &frame, K_MSEC(CONFIG_ZELOS_J1939_TICK_MS)) == 0;
		uint32_t now = k_uptime_get_32();

		k_mutex_lock(&lock, K_FOREVER);
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
	}
}

int zelos_j1939_start(struct zelos_j1939 *j)
{
	static const uint32_t pgns[] = {ZELOS_J1939_PGN_REQUEST, ZELOS_J1939_PGN_ADDRESS_CLAIMED};
	int err;

	j->send = send_frame;
	err = zelos_j1939_init(j, k_uptime_get_32());
	if (err < 0) {
		LOG_ERR("a message has a PDU1 PGN");
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(pgns); i++) {
		err = zelos_can_subscribe_ext(pgns[i] << 8, PF_MASK, &rx_msgq);
		if (err < 0) {
			LOG_ERR("subscribe to PGN %u: %d", pgns[i], err);
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
