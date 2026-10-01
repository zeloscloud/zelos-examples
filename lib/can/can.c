#include <zelos/can.h>

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_can, LOG_LEVEL_INF);

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

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

	LOG_INF("CAN up at %d bit/s", CONFIG_ZELOS_CAN_BITRATE);

	return 0;
}

static int send_frame(uint32_t id, uint8_t flags, const uint8_t *data, uint8_t len)
{
	struct can_frame frame = {.id = id, .flags = flags};
	int err;

	if (len > sizeof(frame.data)) {
		return -EINVAL;
	}

	frame.dlc = can_bytes_to_dlc(len);
	memcpy(frame.data, data, len);

	/*
	 * A bounded timeout rather than K_FOREVER, so a node that cannot
	 * transmit keeps its own schedule instead of stalling in the driver.
	 */
	err = can_send(can_dev, &frame, K_MSEC(10), NULL, NULL);
	if (err != 0) {
		LOG_WRN("send of 0x%03x: %d", id, err);
	}

	return err;
}

int zelos_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
	return send_frame(id, 0, data, len);
}

int zelos_can_send_ext(uint32_t id, const uint8_t *data, uint8_t len)
{
	return send_frame(id, CAN_FRAME_IDE, data, len);
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
