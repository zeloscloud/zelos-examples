#include <bench/bench_can.h>

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bench_can, LOG_LEVEL_INF);

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

int bench_can_init(void)
{
	int err;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	/*
	 * Set the bitrate explicitly. Zephyr's default when the devicetree is
	 * silent is 125 kbit/s, and Renode models no arbitration, so nodes at
	 * different bitrates would work in simulation and fail on hardware.
	 */
	err = can_set_bitrate(can_dev, BENCH_CAN_BITRATE);
	if (err != 0) {
		LOG_ERR("can_set_bitrate: %d", err);
		return err;
	}

	err = can_start(can_dev);
	if (err != 0) {
		LOG_ERR("can_start: %d", err);
		return err;
	}

	LOG_INF("CAN up at %d bit/s", BENCH_CAN_BITRATE);

	return 0;
}

int bench_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
	struct can_frame frame = {.id = id};
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

int bench_can_subscribe(uint32_t id, struct k_msgq *msgq)
{
	const struct can_filter filter = {
		.id = id,
		.mask = CAN_STD_ID_MASK,
	};

	/* The msgq variant runs in thread context; the callback variant is an ISR. */
	return can_add_rx_filter_msgq(can_dev, msgq, &filter);
}
