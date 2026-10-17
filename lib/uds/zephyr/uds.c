/*
 * UDS on ISO-TP: the core on Zephyr's ISO-TP stack, which binds its own CAN
 * filters on the controller lib/can started. Physical and functional requests
 * share one thread; every response leaves on the response identifier.
 *
 * ISO-TP sends straight to the driver, not through lib/can's queue: its state
 * machine runs on each frame's own completion callback, which the queue does
 * not carry. The controller's TX buffers are a priority queue (M_CAN queue
 * mode), so 0x7E8 still yields to every lower identifier, as it would in the
 * queue; it only waits on the driver for a free buffer like the queue's
 * thread, up to ISO-TP's N_As (CONFIG_ISOTP_A_TIMEOUT). Its frames are not in
 * zelos_can_stats().
 */

#include <zelos/reboot.h>
#include <zelos/uds.h>

#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(zelos_uds, LOG_LEVEL_INF);

#define UDS_STACK_SIZE 2048
/* Below the application and XCP, so diagnostics never delay either. */
#define UDS_PRIORITY 6
/* The core's timers run at this resolution. */
#define UDS_TICK_MS 10

BUILD_ASSERT(CONFIG_ISOTP_RX_BUF_COUNT * CONFIG_ISOTP_RX_BUF_SIZE >= CONFIG_ZELOS_UDS_BUF_SIZE,
	     "ISO-TP's receive pool cannot hold the largest request");
BUILD_ASSERT(IS_ENABLED(CONFIG_CSPRNG_ENABLED), "security seeds need a hardware entropy source");

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static const struct isotp_msg_id phys_addr = {.std_id = CONFIG_ZELOS_UDS_PHYS_ID};
static const struct isotp_msg_id func_addr = {.std_id = CONFIG_ZELOS_UDS_FUNC_ID};
static const struct isotp_msg_id resp_addr = {.std_id = CONFIG_ZELOS_UDS_RESP_ID};
/* Any block size, no separation time: the node keeps up with the bus. */
static const struct isotp_fc_opts fc_opts = {.bs = 0, .stmin = 0};

static struct isotp_recv_ctx phys_ctx;
/* Whether phys_ctx holds the physical identifier's one filter. See send(). */
static bool phys_bound;
static struct isotp_recv_ctx func_ctx;
static struct isotp_send_ctx send_ctx;

/* The core is shared by the UDS thread (requests) and the caller of zelos_uds_dtc(). */
static K_MUTEX_DEFINE(core_lock);
static struct zelos_uds_core core;
/* Kept across a reset: startup leaves .noinit alone. Lost at power-off. */
static __noinit struct zelos_uds_dtc_memory dtc_memory;
static uint8_t req[CONFIG_ZELOS_UDS_BUF_SIZE];
static uint8_t rsp[CONFIG_ZELOS_UDS_BUF_SIZE];

static uint32_t seed_random(void)
{
	uint32_t v;

	if (sys_csrand_get(&v, sizeof(v)) != 0) {
		/* Never stall the core's retry on a zero; a weak seed is logged. */
		LOG_ERR("entropy source failed");
		return k_cycle_get_32() | 1U;
	}
	return v;
}

static void bind_phys(k_timeout_t timeout)
{
	int err = isotp_bind(&phys_ctx, can_dev, &phys_addr, &resp_addr, &fc_opts, timeout);

	phys_bound = err == ISOTP_N_OK;
	if (!phys_bound) {
		LOG_ERR("bind 0x%03x: %d", CONFIG_ZELOS_UDS_PHYS_ID, err);
	}
}

static K_SEM_DEFINE(sent_sem, 0, 1);
static int sent_err;

/*
 * In the system workqueue, right after the response's last frame went out and
 * isotp_send() removed its flow control filter: the request context takes the
 * identifier back before a tester can have sent its next request.
 */
static void sent(int err, void *rebind)
{
	if (rebind != NULL) {
		bind_phys(K_NO_WAIT);
	}
	sent_err = err;
	k_sem_give(&sent_sem);
}

static void send(size_t len)
{
	/* A first frame and consecutive frames; a single frame needs no flow control. */
	bool multi = len > CAN_MAX_DLEN - 1;
	int err;

	if (len == 0) {
		return;
	}
	/*
	 * The tester's flow control arrives on the physical identifier, which
	 * isotp_send() filters for the response's duration. A controller hands a
	 * frame to one filter only (M_CAN: the first match; can_mcan.c dispatches
	 * on the frame's filter index), so the request context lets go of that
	 * identifier meanwhile: one filter per identifier, always. So the server
	 * is half-duplex, as many ECUs are: a physical request mid-response
	 * aborts the response (N_UNEXP_PDU) and is not received.
	 */
	if (multi && phys_bound) {
		isotp_unbind(&phys_ctx);
		phys_bound = false;
	}
	err = isotp_send(&send_ctx, can_dev, rsp, len, &resp_addr, &phys_addr, sent,
			 multi ? &phys_ctx : NULL);
	if (err == ISOTP_N_OK) {
		/* ISO-TP's own timeouts bound the wait. */
		k_sem_take(&sent_sem, K_FOREVER);
		err = sent_err;
	} else if (multi) {
		bind_phys(K_NO_WAIT);
	}
	if (err != ISOTP_N_OK) {
		LOG_WRN("response 0x%02x: %d", rsp[0], err);
	}
}

static void handle(struct net_buf *buf, bool functional)
{
	size_t len = net_buf_frags_len(buf);
	size_t n;
	uint8_t reset;

	if (functional && len > CAN_MAX_DLEN - 1) {
		/* ISO 15765-2: functional requests are single frames; drop the rest. */
		net_buf_unref(buf);
		return;
	}
	if (len > sizeof(req)) {
		/* Larger than any request this server takes. */
		rsp[0] = 0x7F;
		rsp[1] = buf->data[0];
		rsp[2] = 0x13;
		net_buf_unref(buf);
		send(3);
		return;
	}
	(void)net_buf_linearize(req, sizeof(req), buf, 0, len);
	net_buf_unref(buf);

	k_mutex_lock(&core_lock, K_FOREVER);
	n = zelos_uds_core_request(&core, req, len, functional, k_uptime_get(), rsp);
	reset = core.reset;
	k_mutex_unlock(&core_lock);

	send(n);

	if (reset != ZELOS_UDS_RESET_NONE) {
		/* After the positive response, which send() waited out. */
		LOG_INF("ECUReset %s", reset == ZELOS_UDS_RESET_HARD ? "hard" : "soft");
		/* The node's one reboot path; seal() runs on the way. */
		zelos_reboot(reset == ZELOS_UDS_RESET_HARD ? SYS_REBOOT_COLD : SYS_REBOOT_WARM);
	}
}

/*
 * zelos_reboot()'s hook, so every orderly reboot keeps the DTC memory, not only
 * ECUReset's. The lock is held through the reboot: nothing changes the memory
 * after its seal.
 */
static void seal(void)
{
	k_mutex_lock(&core_lock, K_FOREVER);
	zelos_uds_core_persist(&core);
}

static void receive(struct isotp_recv_ctx *ctx, k_timeout_t timeout, bool functional)
{
	struct net_buf *buf;
	int ret = ISOTP_RECV_TIMEOUT;

	/* A response may unbind and rebind phys_ctx; stop if it could not. */
	while ((functional || phys_bound) && (ret = isotp_recv_net(ctx, &buf, timeout)) >= 0) {
		handle(buf, functional);
		timeout = K_NO_WAIT;
	}
	if (!functional && !phys_bound) {
		return;
	}
	if (ret != ISOTP_RECV_TIMEOUT) {
		LOG_WRN("%s request: %d", functional ? "functional" : "physical", ret);
	}
}

static void uds_thread(void *p1, void *p2, void *p3)
{
	size_t n;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		/* Wakes at once on a physical request; functional ones wait a tick at most. */
		if (!phys_bound) {
			bind_phys(K_MSEC(100));
		}
		if (phys_bound) {
			receive(&phys_ctx, K_MSEC(UDS_TICK_MS), false);
		} else {
			k_sleep(K_MSEC(UDS_TICK_MS));
		}
		receive(&func_ctx, K_NO_WAIT, true);

		k_mutex_lock(&core_lock, K_FOREVER);
		n = zelos_uds_core_tick(&core, k_uptime_get(), rsp);
		k_mutex_unlock(&core_lock);
		send(n);
	}
}

K_THREAD_DEFINE(uds_tid, UDS_STACK_SIZE, uds_thread, NULL, NULL, NULL, UDS_PRIORITY, 0,
		SYS_FOREVER_MS);

int zelos_uds_start(const struct zelos_uds_config *config)
{
	int err;

	err = zelos_uds_core_init(&core, config, seed_random, &dtc_memory, 0, k_uptime_get());
	if (err < 0) {
		LOG_ERR("configuration: %d", err);
		return err;
	}

	bind_phys(K_NO_WAIT);
	if (!phys_bound) {
		return -EIO;
	}
	/*
	 * Functional requests are single frames: a first frame there still gets
	 * flow control from the stack, then handle() drops the request.
	 */
	err = isotp_bind(&func_ctx, can_dev, &func_addr, &resp_addr, &fc_opts, K_NO_WAIT);
	if (err != ISOTP_N_OK) {
		LOG_ERR("bind 0x%03x: %d", CONFIG_ZELOS_UDS_FUNC_ID, err);
		isotp_unbind(&phys_ctx);
		return -EIO;
	}

	zelos_reboot_on(seal);
	k_thread_start(uds_tid);
	LOG_INF("UDS on 0x%03x (functional 0x%03x), responses 0x%03x", CONFIG_ZELOS_UDS_PHYS_ID,
		CONFIG_ZELOS_UDS_FUNC_ID, CONFIG_ZELOS_UDS_RESP_ID);

	return 0;
}

void zelos_uds_dtc(size_t index, bool failed)
{
	k_mutex_lock(&core_lock, K_FOREVER);
	zelos_uds_core_dtc(&core, index, failed);
	k_mutex_unlock(&core_lock);
}

/* One byte, written by the UDS thread under the lock: read without it. */
bool zelos_uds_tx_enabled(void)
{
	return core.communication == 0x00;
}

bool zelos_uds_rx_enabled(void)
{
	return core.communication != 0x03;
}
