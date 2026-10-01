/*
 * XCP on CAN: the core on lib/can. CROs arrive on one identifier, everything
 * the slave sends leaves on another.
 */

#include <zelos/can.h>
#include <zelos/xcp.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_xcp, LOG_LEVEL_INF);

#define XCP_STACK_SIZE 1024
/* Below the application, so XCP traffic never delays its cycle. */
#define XCP_PRIORITY 5

K_MSGQ_DEFINE(xcp_cro_msgq, sizeof(struct can_frame), 4, 4);
static K_SEM_DEFINE(dto_sem, 0, 1);
/* The core is shared by the XCP thread (commands) and the caller of events. */
static K_MUTEX_DEFINE(core_lock);
static struct zelos_xcp_core core;

static void send_pending(void)
{
	struct zelos_xcp_frame frame;
	bool more;

	for (;;) {
		k_mutex_lock(&core_lock, K_FOREVER);
		more = zelos_xcp_core_poll(&core, &frame);
		k_mutex_unlock(&core_lock);
		if (!more) {
			return;
		}
		/* Outside the lock: a slow bus must not block zelos_xcp_event(). */
		(void)zelos_can_send(CONFIG_ZELOS_XCP_DTO_ID, frame.data, frame.len);
	}
}

static void xcp_thread(void *p1, void *p2, void *p3)
{
	struct k_poll_event events[] = {
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
					 K_POLL_MODE_NOTIFY_ONLY, &xcp_cro_msgq),
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					 &dto_sem),
	};
	struct can_frame frame;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_poll(events, ARRAY_SIZE(events), K_FOREVER);
		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
		(void)k_sem_take(&dto_sem, K_NO_WAIT);

		while (k_msgq_get(&xcp_cro_msgq, &frame, K_NO_WAIT) == 0) {
			k_mutex_lock(&core_lock, K_FOREVER);
			zelos_xcp_core_on_frame(&core, frame.data, can_dlc_to_bytes(frame.dlc));
			k_mutex_unlock(&core_lock);
			send_pending();
		}
		send_pending();
	}
}

K_THREAD_DEFINE(xcp_tid, XCP_STACK_SIZE, xcp_thread, NULL, NULL, NULL, XCP_PRIORITY, 0,
		SYS_FOREVER_MS);

int zelos_xcp_start(const struct zelos_xcp_config *config)
{
	int err;

	zelos_xcp_core_init(&core, config);

	err = zelos_can_subscribe(CONFIG_ZELOS_XCP_CRO_ID, &xcp_cro_msgq);
	if (err < 0) {
		LOG_ERR("subscribe to CRO 0x%03x: %d", CONFIG_ZELOS_XCP_CRO_ID, err);
		return err;
	}

	k_thread_start(xcp_tid);
	LOG_INF("XCP on CRO 0x%03x, DTO 0x%03x", CONFIG_ZELOS_XCP_CRO_ID,
		CONFIG_ZELOS_XCP_DTO_ID);

	return 0;
}

void zelos_xcp_event(uint8_t channel)
{
	k_mutex_lock(&core_lock, K_FOREVER);
	zelos_xcp_core_event(&core, channel);
	k_mutex_unlock(&core_lock);
	k_sem_give(&dto_sem);
}
