/*
 * The AUTOSAR PDU schedule on lib/can: one thread sends each entry on its
 * deadline, the earliest first.
 */

#include <zelos/autosar.h>
#include <zelos/can.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_autosar, LOG_LEVEL_INF);

/* Below the application, above XCP (5) and UDS (6): cyclic COM is application traffic. */
#define AUTOSAR_PRIORITY 3

static const struct zelos_ar_tx *schedule;
static size_t entries;
static int64_t deadline[CONFIG_ZELOS_AUTOSAR_TX_MAX];

static void autosar_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		const int64_t now = k_uptime_get();
		int64_t next = INT64_MAX;

		for (size_t i = 0; i < entries; i++) {
			if (deadline[i] <= now) {
				uint8_t data[CAN_MAX_DLEN];
				const uint8_t len = schedule[i].build(data);

				if (len > 0) {
					(void)zelos_can_send(schedule[i].id, data, len);
				}
				/* Missed cycles are skipped, keeping the phase. */
				while (deadline[i] <= now) {
					deadline[i] += schedule[i].period_ms;
				}
			}
			next = MIN(next, deadline[i]);
		}
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(autosar_tid, CONFIG_ZELOS_AUTOSAR_STACK_SIZE, autosar_thread, NULL, NULL, NULL,
		AUTOSAR_PRIORITY, 0, SYS_FOREVER_MS);

int zelos_autosar_start(const struct zelos_ar_tx *tx, size_t count)
{
	const int64_t start = k_uptime_get();

	if (count == 0 || count > ARRAY_SIZE(deadline)) {
		LOG_ERR("%zu PDUs; 1..%d fit", count, CONFIG_ZELOS_AUTOSAR_TX_MAX);
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		if (tx[i].period_ms == 0) {
			LOG_ERR("PDU 0x%03x has no period", tx[i].id);
			return -EINVAL;
		}
		deadline[i] = start + tx[i].offset_ms;
	}
	schedule = tx;
	entries = count;

	k_thread_start(autosar_tid);
	LOG_INF("AUTOSAR: %zu PDUs", count);

	return 0;
}
