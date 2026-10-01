#include <zelos/canopen.h>

#include <canopennode.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(zelos_canopen, LOG_LEVEL_INF);

/* Wakes the loop on reception, so SDO and NMT are not held to its period. */
static K_SEM_DEFINE(rx_sem, 0, 1);

static void on_rx(void)
{
	k_sem_give(&rx_sem);
}

int zelos_canopen_run(void (*tick)(void))
{
	struct canopen_context can = {.dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))};
	CO_NMT_reset_cmd_t reset = CO_RESET_NOT;

	if (!device_is_ready(can.dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	canopen_set_rxmsg_callback(on_rx);

	/* Reset-communication comes back here; CO_init reuses its allocation. */
	while (reset != CO_RESET_APP) {
		CO_ReturnError_t err;
		uint32_t elapsed_ms = 0U;
		int64_t stamp;

		/* The stack takes kbit/s. */
		err = CO_init(&can, CONFIG_ZELOS_CANOPEN_NODE_ID, CONFIG_ZELOS_CAN_BITRATE / 1000);
		if (err != CO_ERROR_NO) {
			LOG_ERR("CO_init: %d", err);
			return -EIO;
		}

		CO_CANsetNormalMode(CO->CANmodule[0]);
		LOG_INF("CANopen node 0x%02x up at %d bit/s", CONFIG_ZELOS_CANOPEN_NODE_ID,
			CONFIG_ZELOS_CAN_BITRATE);

		stamp = k_uptime_get();

		do {
			uint16_t timeout_ms = 1U;

			reset = CO_process(CO, (uint16_t)elapsed_ms, &timeout_ms);

			CO_LOCK_OD();
			tick();
			CO_UNLOCK_OD();

			(void)k_sem_take(&rx_sem, K_MSEC(timeout_ms));
			/* From one stamp, so sub-millisecond wakeups do not lose time. */
			elapsed_ms = (uint32_t)k_uptime_delta(&stamp);
		} while (reset == CO_RESET_NOT);
	}

	LOG_INF("NMT reset-node");
	CO_delete(&can);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}
