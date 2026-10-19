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
 *
 * Zephyr's ISO-TP, used as is, cannot abort a send, and with nothing on the
 * bus to acknowledge a frame it never ends one: send() bounds the wait and
 * restarts the controller, so the thread goes on and S3 ends the session.
 *
 * With CONFIG_ZELOS_UDS_NVS the permanent DTCs, as their 3-byte numbers, are
 * one entry of Zephyr Memory Storage (ZMS) on the storage partition, mounted
 * and read, then rewritten whenever the set changes, by a thread of the lowest
 * application priority, so a flash write or erase (a blank partition's first
 * mount erases a 128 KB sector) never delays the application, XCP or this
 * server. Until the mount, the permanent DTCs are those in RAM. A store
 * that does not mount, or a write that fails, is ZELOS_UDS_DTC_NV_ERROR: the
 * core then runs on nv_config, the node's DTCs and that one after them.
 */

#include <zelos/can.h>
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

#if defined(CONFIG_ZELOS_UDS_NVS)
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>

#include <string.h>
#endif

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

#if defined(CONFIG_ZELOS_UDS_NVS)
#define NVS_PARTITION storage_partition
#define NVS_ID_PERMANENT 1
#define NVS_STACK_SIZE 1024

/* ZMS, not NVS: NVS takes sectors of 64 KB at most, an H7 sector is 128 KB. */
static struct zms_fs nvs = {
	.flash_device = PARTITION_DEVICE(NVS_PARTITION),
	.offset = PARTITION_OFFSET(NVS_PARTITION),
};
static bool nvs_ready;
/* The node's configuration with ZELOS_UDS_DTC_NV_ERROR appended; dtc_count 0 if it is full. */
static struct zelos_uds_config nv_config;
static uint32_t nv_dtcs[ZELOS_UDS_DTCS_MAX];
/* The node's configuration, without the store's DTC. */
static const struct zelos_uds_config *nvs_node;
/* The permanent set flash holds, as a mask over the configuration's DTCs. */
static atomic_t nvs_saved;
static struct k_work_q nvs_queue;
/* Held through a flash write; seal() takes it so a reboot never cuts one. */
static K_MUTEX_DEFINE(nvs_lock);
static K_THREAD_STACK_DEFINE(nvs_stack, NVS_STACK_SIZE);

/* The node's DTCs and, after them, the store's own. */
static void nvs_prepare(const struct zelos_uds_config *config)
{
	nvs_node = config;
	if (config->dtc_count < ZELOS_UDS_DTCS_MAX) {
		nv_config = *config;
		memcpy(nv_dtcs, config->dtcs, config->dtc_count * sizeof(nv_dtcs[0]));
		nv_dtcs[config->dtc_count] = ZELOS_UDS_DTC_NV_ERROR;
		nv_config.dtcs = nv_dtcs;
		nv_config.dtc_count = config->dtc_count + 1;
	} else {
		LOG_WRN("no room for DTC 0x%06x", ZELOS_UDS_DTC_NV_ERROR);
	}
}

/* Under nvs_lock: mount; the permanent DTCs flash holds, as a mask, in *mask. */
static bool nvs_load(uint32_t *mask)
{
	struct flash_pages_info page;
	uint32_t codes[ZELOS_UDS_DTCS_MAX];
	ssize_t n;

	*mask = 0;
	if (!device_is_ready(nvs.flash_device) ||
	    flash_get_page_info_by_offs(nvs.flash_device, nvs.offset, &page) != 0) {
		LOG_ERR("no flash: permanent DTCs are lost at power-off");
		return false;
	}
	nvs.sector_size = page.size;
	nvs.sector_count = PARTITION_SIZE(NVS_PARTITION) / page.size;
	if (zms_mount(&nvs) != 0) {
		LOG_ERR("flash storage: permanent DTCs are lost at power-off");
		return false;
	}
	n = zms_read(&nvs, NVS_ID_PERMANENT, codes, sizeof(codes));
	for (ssize_t i = 0; i < MIN(n, (ssize_t)sizeof(codes)) / (ssize_t)sizeof(codes[0]); i++) {
		/* A DTC this build no longer has is dropped. */
		for (size_t j = 0; j < nvs_node->dtc_count; j++) {
			*mask |= nvs_node->dtcs[j] == codes[i] ? BIT(j) : 0;
		}
	}
	atomic_set(&nvs_saved, (atomic_val_t)*mask);
	return true;
}

/*
 * Under core_lock: the store's test result. A failure brings its DTC into the
 * core; a pass reaches it only once it is there.
 */
static void nv_report(bool failed)
{
	if (nv_config.dtc_count == 0) {
		return;
	}
	if (core.config != &nv_config) {
		if (!failed) {
			return;
		}
		zelos_uds_core_extend(&core, &nv_config);
	}
	zelos_uds_core_dtc(&core, nv_config.dtc_count - 1, failed);
}

/* Under core_lock: the permanent set flash must hold, the node's DTCs only. */
static uint32_t nvs_permanent(void)
{
	return zelos_uds_core_permanent(&core) & BIT_MASK(nvs_node->dtc_count);
}

static void nvs_store(struct k_work *work)
{
	uint32_t codes[ZELOS_UDS_DTCS_MAX];
	uint32_t mask;
	size_t n = 0;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&core_lock, K_FOREVER);
	mask = nvs_permanent();
	k_mutex_unlock(&core_lock);
	for (size_t i = 0; i < nvs_node->dtc_count; i++) {
		if (mask & BIT(i)) {
			codes[n++] = core.config->dtcs[i];
		}
	}
	k_mutex_lock(&nvs_lock, K_FOREVER);
	err = n > 0 ? (int)zms_write(&nvs, NVS_ID_PERMANENT, codes, n * sizeof(codes[0]))
		    : zms_delete(&nvs, NVS_ID_PERMANENT);
	k_mutex_unlock(&nvs_lock);
	if (err < 0) {
		LOG_ERR("permanent DTCs not stored: %d", err);
	} else {
		atomic_set(&nvs_saved, (atomic_val_t)mask);
	}
	/* After nvs_lock: seal() takes core_lock first. */
	k_mutex_lock(&core_lock, K_FOREVER);
	nv_report(err < 0);
	k_mutex_unlock(&core_lock);
}

static K_WORK_DEFINE(nvs_work, nvs_store);

/* Under core_lock: queue a write if the permanent set moved from flash's. */
static void nvs_sync(void)
{
	if (nvs_ready && nvs_permanent() != (uint32_t)atomic_get(&nvs_saved)) {
		k_work_submit_to_queue(&nvs_queue, &nvs_work);
	}
}

/*
 * The mount, after the node's threads have started: the store's result, then
 * flash's permanent DTCs if a power loss cleared RAM's, then a write of any the
 * new cycle released. Writes queue behind it.
 */
static void nvs_mount(struct k_work *work)
{
	uint32_t permanent;
	bool mounted;

	ARG_UNUSED(work);

	k_mutex_lock(&nvs_lock, K_FOREVER);
	mounted = nvs_load(&permanent);
	k_mutex_unlock(&nvs_lock);
	/* After nvs_lock: seal() takes core_lock first. */
	k_mutex_lock(&core_lock, K_FOREVER);
	nvs_ready = mounted;
	zelos_uds_core_restore(&core, permanent);
	nv_report(!mounted);
	nvs_sync();
	k_mutex_unlock(&core_lock);
}

static K_WORK_DEFINE(nvs_mount_work, nvs_mount);

/* What the core starts on: with the store's DTC if the memory kept it. */
static const struct zelos_uds_config *nv_start_config(const struct zelos_uds_config *config)
{
	nvs_prepare(config);
	if (nv_config.dtc_count != 0 && zelos_uds_core_kept(&dtc_memory, config->dtc_count)) {
		return &nv_config;
	}
	return config;
}

/*
 * Before core_lock, which both take: finish the mount, so the memory sealed
 * holds flash's permanent DTCs, then the write queued or running.
 */
static void nvs_flush(void)
{
	struct k_work_sync sync;

	(void)k_work_flush(&nvs_mount_work, &sync);
	(void)k_work_flush(&nvs_work, &sync);
}

/* Under core_lock: wait out a write begun since the flush; none begins after. */
static void nvs_quiesce(void)
{
	k_mutex_lock(&nvs_lock, K_FOREVER);
}
#else
static void nvs_sync(void)
{
}

static const struct zelos_uds_config *nv_start_config(const struct zelos_uds_config *config)
{
	return config;
}

static void nvs_flush(void)
{
}

static void nvs_quiesce(void)
{
}
#endif

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

/* ISO 15765-2: the longest message a classic first frame announces; the largest STmin. */
#define ISOTP_MAX_LEN 4095
#define STMIN_MAX_MS  127
/* Above the slowest send the spec allows: CFs at STmin max, N_WFTmax waits + FC at N_Bs, N_As. */
#define SEND_BACKSTOP_MS                                                                           \
	(DIV_ROUND_UP(ISOTP_MAX_LEN - 6, 7) * STMIN_MAX_MS +                                       \
	 (CONFIG_ISOTP_WFTMAX + 1) * CONFIG_ISOTP_BS_TIMEOUT + CONFIG_ISOTP_A_TIMEOUT)
/* With the controller stopped every frame fails, so a send ends within N_Bs, its longest timer. */
#define DRAIN_MS (CONFIG_ISOTP_BS_TIMEOUT + STMIN_MAX_MS)

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

/*
 * Drops the controller's frames; their callbacks have run once it returns.
 * ISO-TP is not waiting in can_send() for a buffer then: each caller's send
 * already failed or ended, or its frame is in the controller.
 */
static void restart(k_timeout_t off)
{
	struct k_work_sync sync;

	(void)zelos_can_restart(off);
	(void)k_work_flush(&send_ctx.work, &sync);
}

/*
 * Whether the send ended, through sent(), before the backstop. When a CF's
 * can_send() fails, ISO-TP's CF path records N_As or N_ERROR and never runs
 * again: no end either. Any other error ends the send through ISO-TP, the CF
 * loop first if it runs (a request mid-response: N_UNEXP_PDU).
 */
static bool wait_sent(void)
{
	for (int waited = 0; waited < SEND_BACKSTOP_MS; waited += CONFIG_ISOTP_A_TIMEOUT) {
		if (k_sem_take(&sent_sem, K_MSEC(CONFIG_ISOTP_A_TIMEOUT)) == 0) {
			return true;
		}
		if (send_ctx.error_nr == ISOTP_N_TIMEOUT_A || send_ctx.error_nr == ISOTP_N_ERROR) {
			/* Unless it ended meanwhile: an end runs in the higher-priority workqueue. */
			return k_sem_take(&sent_sem, K_NO_WAIT) == 0;
		}
	}
	return false;
}

/* A send with no end: stopped, every frame fails, so it ends or stays where it failed. */
static void abort_send(bool multi)
{
	LOG_ERR("response 0x%02x: no end (%d), restarting CAN", rsp[0], (int)send_ctx.error_nr);
	restart(K_MSEC(DRAIN_MS));
	if (k_sem_take(&sent_sem, K_NO_WAIT) != 0 && multi) {
		/* Its flow control filter still holds the physical identifier. */
		can_remove_rx_filter(can_dev, send_ctx.filter_id);
		bind_phys(K_NO_WAIT);
	}
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
		if (!wait_sent()) {
			abort_send(multi);
			return;
		}
		err = sent_err;
	} else if (multi) {
		bind_phys(K_NO_WAIT);
	}
	if (err == ISOTP_N_TIMEOUT_A || err == ISOTP_N_TIMEOUT_BS) {
		/* The frame nothing acknowledged may still be in the controller. */
		LOG_ERR("response 0x%02x: %d, restarting CAN", rsp[0], err);
		restart(K_NO_WAIT);
	} else if (err != ISOTP_N_OK) {
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
 * ECUReset's. The locks are held through the reboot: nothing changes the memory
 * after its seal, and no flash write is cut short.
 */
static void seal(void)
{
	nvs_flush();
	k_mutex_lock(&core_lock, K_FOREVER);
	nvs_quiesce();
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

	/* Flash's permanent DTCs come with the mount (nvs_mount()). */
	err = zelos_uds_core_init(&core, nv_start_config(config), seed_random, &dtc_memory, 0,
				  k_uptime_get());
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
#if defined(CONFIG_ZELOS_UDS_NVS)
	k_work_queue_start(&nvs_queue, nvs_stack, K_THREAD_STACK_SIZEOF(nvs_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_work_submit_to_queue(&nvs_queue, &nvs_mount_work);
#endif
	LOG_INF("UDS on 0x%03x (functional 0x%03x), responses 0x%03x", CONFIG_ZELOS_UDS_PHYS_ID,
		CONFIG_ZELOS_UDS_FUNC_ID, CONFIG_ZELOS_UDS_RESP_ID);

	return 0;
}

void zelos_uds_dtc(size_t index, bool failed)
{
	k_mutex_lock(&core_lock, K_FOREVER);
	zelos_uds_core_dtc(&core, index, failed);
	nvs_sync();
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
