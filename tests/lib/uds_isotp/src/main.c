/*
 * The UDS server on Zephyr's ISO-TP, against a controller with M_CAN's filter
 * semantics: a received frame goes to the first matching filter only. On the
 * STM32H7's FDCAN, a second filter on the physical request identifier never
 * saw the tester's flow control, so every multi-frame response stalled.
 */

#include <zelos/uds.h>

#include <string.h>

#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;

#define VIN "1ZLSDCDC0BENCH001"
#define PAD 0xCC

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static void read_vin(uint8_t *out)
{
	memcpy(out, VIN, sizeof(VIN) - 1);
}

static const struct zelos_uds_did dids[] = {{0xF190, sizeof(VIN) - 1, read_vin, NULL}};
static const struct zelos_uds_config config = {.dids = dids, .did_count = ARRAY_SIZE(dids)};

/* The controller's standard filter elements, matched in index order. */
static struct {
	bool used;
	struct can_filter filter;
	can_rx_callback_t cb;
	void *user_data;
} filters[16];
/* Times two filters covered a common identifier. */
static int overlaps;

static int add_rx_filter(const struct device *dev, can_rx_callback_t cb, void *user_data,
			 const struct can_filter *filter)
{
	int free = -1;

	ARG_UNUSED(dev);

	for (int i = 0; i < ARRAY_SIZE(filters); i++) {
		const struct can_filter *f = &filters[i].filter;

		if (!filters[i].used) {
			free = free < 0 ? i : free;
		} else if (((filter->id ^ f->id) & filter->mask & f->mask) == 0) {
			overlaps++;
		}
	}
	if (free >= 0) {
		filters[free].used = true;
		filters[free].filter = *filter;
		filters[free].cb = cb;
		filters[free].user_data = user_data;
	}
	return free >= 0 ? free : -ENOSPC;
}

static void remove_rx_filter(const struct device *dev, int id)
{
	ARG_UNUSED(dev);
	filters[id].used = false;
}

/* Frames the server sent, with their completion callbacks, completed by the bus thread. */
struct sent {
	struct can_frame frame;
	can_tx_callback_t cb;
	void *user_data;
};
K_MSGQ_DEFINE(sent_q, sizeof(struct sent), 16, 4);
K_MSGQ_DEFINE(tester_q, sizeof(struct can_frame), 16, 4);

static int send(const struct device *dev, const struct can_frame *frame, k_timeout_t timeout,
		can_tx_callback_t cb, void *user_data)
{
	struct sent s = {.frame = *frame, .cb = cb, .user_data = user_data};

	ARG_UNUSED(dev);
	return k_msgq_put(&sent_q, &s, timeout);
}

static void bus(void *p1, void *p2, void *p3)
{
	struct sent s;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_msgq_get(&sent_q, &s, K_FOREVER);
		if (s.cb != NULL) {
			s.cb(can_dev, 0, s.user_data);
		}
		(void)k_msgq_put(&tester_q, &s.frame, K_FOREVER);
	}
}

K_THREAD_DEFINE(bus_tid, 1024, bus, NULL, NULL, NULL, 2, 0, 0);

/* The tester transmits: the first matching filter receives, as on M_CAN. */
static void deliver(uint32_t id, const uint8_t *data, uint8_t len)
{
	struct can_frame frame = {.id = id, .dlc = 8};

	memset(frame.data, PAD, sizeof(frame.data));
	memcpy(frame.data, data, len);
	for (int i = 0; i < ARRAY_SIZE(filters); i++) {
		const struct can_filter *f = &filters[i].filter;

		if (filters[i].used && (f->flags & CAN_FILTER_IDE) == 0 &&
		    ((id ^ f->id) & f->mask) == 0) {
			filters[i].cb(can_dev, &frame, filters[i].user_data);
			return;
		}
	}
}

static struct can_frame received(void)
{
	struct can_frame frame;

	zassert_ok(k_msgq_get(&tester_q, &frame, K_MSEC(3000)), "no frame from the server");
	zassert_equal(frame.id, CONFIG_ZELOS_UDS_RESP_ID);
	return frame;
}

ZTEST(uds_isotp, test_multi_frame_response_then_more_requests)
{
	static const uint8_t expected[] = {0x62, 0xF1, 0x90, '1', 'Z', 'L', 'S', 'D', 'C', 'D',
					   'C', '0', 'B', 'E', 'N', 'C', 'H', '0', '0', '1'};
	uint8_t response[sizeof(expected)];
	struct can_frame f;
	size_t n;

	fake_can_add_rx_filter_fake.custom_fake = add_rx_filter;
	fake_can_remove_rx_filter_fake.custom_fake = remove_rx_filter;
	fake_can_send_fake.custom_fake = send;
	zassert_ok(zelos_uds_start(&config));

	for (int round = 0; round < 2; round++) {
		deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x03, 0x22, 0xF1, 0x90}, 4);
		f = received();
		zassert_equal(f.data[0], 0x10, "first frame");
		zassert_equal(f.data[1], sizeof(expected));
		memcpy(response, &f.data[2], 6);
		n = 6;
		/* Flow control on the physical identifier: continue, no limits. */
		deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x30, 0x00, 0x00}, 3);
		for (uint8_t sn = 1; n < sizeof(expected); sn++) {
			f = received();
			zassert_equal(f.data[0], 0x20 | sn, "consecutive frame %u", sn);
			memcpy(&response[n], &f.data[1], MIN(7, sizeof(expected) - n));
			n += MIN(7, sizeof(expected) - n);
		}
		zassert_mem_equal(response, expected, sizeof(expected));

		/* The request context is back on its identifier. */
		deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x02, 0x3E, 0x00}, 3);
		f = received();
		zassert_mem_equal(f.data, ((uint8_t[]){0x02, 0x7E, 0x00}), 3);
	}

	zassert_equal(overlaps, 0, "two filters covered one identifier");
}

ZTEST_SUITE(uds_isotp, NULL, NULL, NULL, NULL, NULL);
