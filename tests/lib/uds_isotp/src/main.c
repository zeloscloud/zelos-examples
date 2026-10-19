/*
 * The UDS server on Zephyr's ISO-TP, against a controller with M_CAN's filter
 * semantics: a received frame goes to the first matching filter only. On the
 * STM32H7's FDCAN, a second filter on the physical request identifier never
 * saw the tester's flow control, so every multi-frame response stalled. And
 * with nothing to acknowledge its frames, a response the server cannot end.
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

/* Nothing acknowledges: a frame stays in the controller until can_stop(). */
static bool unacked;
/* The controller's TX buffer stays taken: can_send() times out. */
static bool full;
static bool stopped;
static struct sent held[4];
static int held_count;

static int send(const struct device *dev, const struct can_frame *frame, k_timeout_t timeout,
		can_tx_callback_t cb, void *user_data)
{
	struct sent s = {.frame = *frame, .cb = cb, .user_data = user_data};

	ARG_UNUSED(dev);
	if (stopped) {
		return -ENETDOWN;
	}
	if (full) {
		return -EAGAIN;
	}
	if (unacked) {
		zassert_true(held_count < ARRAY_SIZE(held));
		held[held_count++] = s;
		return 0;
	}
	return k_msgq_put(&sent_q, &s, timeout);
}

/* As M_CAN's: every frame held fails to its sender. */
static int stop(const struct device *dev)
{
	stopped = true;
	while (held_count > 0) {
		struct sent s = held[--held_count];

		s.cb(dev, -ENETDOWN, s.user_data);
	}
	return 0;
}

static int start(const struct device *dev)
{
	ARG_UNUSED(dev);
	stopped = false;
	return 0;
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

static const uint8_t vin_rsp[] = {0x62, 0xF1, 0x90, '1', 'Z', 'L', 'S', 'D', 'C', 'D',
				  'C', '0', 'B', 'E', 'N', 'C', 'H', '0', '0', '1'};

static void request_vin(void)
{
	deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x03, 0x22, 0xF1, 0x90}, 4);
}

/* Flow control on the physical identifier: continue, no limits. */
static void continue_to_send(void)
{
	deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x30, 0x00, 0x00}, 3);
}

static struct can_frame first_frame(void)
{
	struct can_frame f = received();

	zassert_equal(f.data[0], 0x10, "first frame");
	zassert_equal(f.data[1], sizeof(vin_rsp));
	return f;
}

/* A multi-frame response, then the request context back on its identifier. */
static void read_vin_by_hand(void)
{
	uint8_t response[sizeof(vin_rsp)];
	struct can_frame f;
	size_t n;

	request_vin();
	f = first_frame();
	memcpy(response, &f.data[2], 6);
	n = 6;
	continue_to_send();
	for (uint8_t sn = 1; n < sizeof(vin_rsp); sn++) {
		f = received();
		zassert_equal(f.data[0], 0x20 | sn, "consecutive frame %u", sn);
		memcpy(&response[n], &f.data[1], MIN(7, sizeof(vin_rsp) - n));
		n += MIN(7, sizeof(vin_rsp) - n);
	}
	zassert_mem_equal(response, vin_rsp, sizeof(vin_rsp));

	deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x02, 0x3E, 0x00}, 3);
	f = received();
	zassert_mem_equal(f.data, ((uint8_t[]){0x02, 0x7E, 0x00}), 3);
}

/* Whether the server answers on its physical identifier within `ms`. */
static bool answers(int ms)
{
	struct can_frame f;

	deliver(CONFIG_ZELOS_UDS_PHYS_ID, (uint8_t[]){0x02, 0x3E, 0x00}, 3);
	return k_msgq_get(&tester_q, &f, K_MSEC(ms)) == 0;
}

/* Back on the bus after a send that could not end: its identifiers its own, sends whole. */
static void recovered(void)
{
	unacked = false;
	full = false;
	zassert_true(answers(100), "no answer after the send");
	read_vin_by_hand();
	zassert_equal(overlaps, 0, "two filters covered one identifier");
}

ZTEST(uds_isotp, test_multi_frame_response_then_more_requests)
{
	for (int round = 0; round < 2; round++) {
		read_vin_by_hand();
	}
	zassert_equal(overlaps, 0, "two filters covered one identifier");
}

/* The first frame never leaves: N_Bs ends the send, the restart drops the frame. */
ZTEST(uds_isotp, test_unacknowledged_first_frame)
{
	unacked = true;
	request_vin();
	k_sleep(K_MSEC(CONFIG_ISOTP_BS_TIMEOUT + 200));
	zassert_equal(held_count, 0, "the first frame is still in the controller");
	recovered();
}

/* A CF finds no free buffer: ISO-TP's CF path records N_As and never ends the send. */
ZTEST(uds_isotp, test_cf_without_a_buffer)
{
	request_vin();
	(void)first_frame();
	full = true;
	continue_to_send();
	/* A slice of N_As to see the error, the drain of N_Bs + 127 ms. */
	k_sleep(K_MSEC(CONFIG_ISOTP_A_TIMEOUT + CONFIG_ISOTP_BS_TIMEOUT + 300));
	recovered();
}

/* A CF never leaves: ISO-TP waits for it for ever, so only the backstop ends the send. */
ZTEST(uds_isotp, test_unacknowledged_cf)
{
	/* The slowest send the spec allows: 585 CFs at STmin 127 ms, N_WFTmax waits + FC. */
	const int slowest_ms = 585 * 127 + (CONFIG_ISOTP_WFTMAX + 1) * CONFIG_ISOTP_BS_TIMEOUT;

	request_vin();
	(void)first_frame();
	unacked = true;
	continue_to_send();
	/* Silent meanwhile: a request would hit the send's FC filter and end it (N_UNEXP_PDU). */
	k_sleep(K_MSEC(slowest_ms));
	zassert_equal(held_count, 1, "restarted before the slowest send the spec allows");
	/* The backstop, its last slice and the drain. */
	k_sleep(K_SECONDS(4));
	zassert_equal(held_count, 0, "the CF is still in the controller");
	recovered();
}

/* The fake controller's own rule resets its fakes before each test. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_can_add_rx_filter_fake.custom_fake = add_rx_filter;
	fake_can_remove_rx_filter_fake.custom_fake = remove_rx_filter;
	fake_can_send_fake.custom_fake = send;
	fake_can_stop_fake.custom_fake = stop;
	fake_can_start_fake.custom_fake = start;
}

static void *setup(void)
{
	before(NULL);
	zassert_ok(zelos_uds_start(&config));
	return NULL;
}

ZTEST_SUITE(uds_isotp, NULL, setup, before, NULL, NULL);
