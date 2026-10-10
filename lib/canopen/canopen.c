#include <zelos/canopen.h>

#include <canopennode.h>
/* After the stack, which defines the types it uses. */
#include <CO_OD.h>

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_canopen, LOG_LEVEL_INF);

/* LSS's 0x7E4/0x7E5 lie in UDS's 0x7E0-0x7EF: never both on one bus. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZELOS_UDS) || (CO_NO_LSS_SERVER == 0 && CO_NO_LSS_CLIENT == 0),
	     "CANopen LSS in an image with UDS");

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
	/*
	 * The dictionary's power-on values, taken here so what the node wrote
	 * before (its build info) is part of them. CiA 301 has reset-communication
	 * restore the communication area (0x1000-0x1FFF, CO_OD_ROM here) and
	 * reset-node every area; the stack leaves both to us.
	 */
	static struct sCO_OD_ROM power_on_rom;
	static struct sCO_OD_RAM power_on_ram;
	static struct sCO_OD_EEPROM power_on_eeprom;

	if (!device_is_ready(can.dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	canopen_set_rxmsg_callback(on_rx);

	memcpy(&power_on_rom, &CO_OD_ROM, sizeof(power_on_rom));
	memcpy(&power_on_ram, &CO_OD_RAM, sizeof(power_on_ram));
	memcpy(&power_on_eeprom, &CO_OD_EEPROM, sizeof(power_on_eeprom));

	/* Both resets come back here; CO_init reuses its allocation. */
	for (;;) {
		CO_ReturnError_t err;
		uint32_t elapsed_ms = 0U;
		int64_t stamp;

		/* The stack's PDO thread must not run on a half-built stack. */
		CO_LOCK_OD();
		if (reset == CO_RESET_APP) {
			memcpy(&CO_OD_RAM, &power_on_ram, sizeof(power_on_ram));
			memcpy(&CO_OD_EEPROM, &power_on_eeprom, sizeof(power_on_eeprom));
		}
		if (reset != CO_RESET_NOT) {
			memcpy(&CO_OD_ROM, &power_on_rom, sizeof(power_on_rom));
		}
		/* The stack takes kbit/s; lib/can has set the bitrate already. */
		err = CO_init(&can, CONFIG_ZELOS_CANOPEN_NODE_ID, CONFIG_ZELOS_CAN_BITRATE / 1000);
		if (err == CO_ERROR_NO) {
			CO_CANsetNormalMode(CO->CANmodule[0]);
		}
		CO_UNLOCK_OD();
		if (err != CO_ERROR_NO) {
			LOG_ERR("CO_init: %d", err);
			return -EIO;
		}
		LOG_INF("CANopen node 0x%02x up", CONFIG_ZELOS_CANOPEN_NODE_ID);

		stamp = k_uptime_get();

		for (;;) {
			uint16_t timeout_ms = 1U;

			reset = CO_process(CO, (uint16_t)elapsed_ms, &timeout_ms);
			if (reset != CO_RESET_NOT) {
				break;
			}

			CO_LOCK_OD();
			tick();
			CO_UNLOCK_OD();

			(void)k_sem_take(&rx_sem, K_MSEC(timeout_ms));
			/* From one stamp, so sub-millisecond wakeups do not lose time. */
			elapsed_ms = (uint32_t)k_uptime_delta(&stamp);
		}

		LOG_INF("NMT %s", reset == CO_RESET_APP ? "reset-node" : "reset-communication");
	}
}
