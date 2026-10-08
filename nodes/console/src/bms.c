/*
 * Battery monitor: warns every five seconds about cell imbalance. A file has
 * one log module, so this cannot live in main.c beside the dcdc module.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bms, LOG_LEVEL_INF);

#define PERIOD K_SECONDS(5)

#define IMBALANCE_MIN_MV 30
#define IMBALANCE_SPAN_MV 21 /* 30 to 50 mV */

static void warn(struct k_timer *timer)
{
	static uint32_t count;

	ARG_UNUSED(timer);

	/* Deterministic, so every run prints the same values. */
	LOG_WRN("cell imbalance %dmV", IMBALANCE_MIN_MV + (int)((count++ * 7U) % IMBALANCE_SPAN_MV));
}

K_TIMER_DEFINE(warn_timer, warn, NULL);

static int bms_start(void)
{
	k_timer_start(&warn_timer, PERIOD, PERIOD);

	return 0;
}

SYS_INIT(bms_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
