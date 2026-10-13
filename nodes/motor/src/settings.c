/*
 * The tuning in flash, through Zephyr's settings subsystem on NVS: tune live
 * from the shell, `pid save`, and the next boot starts from those gains.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "motor.h"

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

static bool loaded;

static int pid_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	struct pid_gains g;

	if (!settings_name_steq(name, "gains", NULL)) {
		return -ENOENT;
	}
	if (len != sizeof(g) || read_cb(cb_arg, &g, sizeof(g)) != sizeof(g)) {
		LOG_WRN("saved tuning has the wrong size, using defaults");
		return -EINVAL;
	}
	control_set_gains(g);
	loaded = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pid, "pid", NULL, pid_set, NULL, NULL);

int tuning_load(void)
{
	int err = settings_subsys_init();

	if (err == 0) {
		err = settings_load();
	}
	if (err != 0) {
		LOG_ERR("settings unavailable, using defaults: %d", err);
	}

	const struct pid_gains g = control_gains();

	LOG_INF("%s kp=%.4f ki=%.3f kd=%.4f", loaded ? "tuning loaded from flash" : "no saved tuning, defaults",
		(double)g.kp, (double)g.ki, (double)g.kd);
	return err;
}

/* Every change of the gains from module config, so they are one set of signals. */
void tuning_report(const char *what)
{
	const struct pid_gains g = control_gains();

	LOG_INF("%s kp=%.4f ki=%.3f kd=%.4f", what, (double)g.kp, (double)g.ki, (double)g.kd);
}

int tuning_save(void)
{
	const struct pid_gains g = control_gains();
	const int err = settings_save_one("pid/gains", &g, sizeof(g));

	if (err != 0) {
		LOG_ERR("saving tuning failed: %d", err);
		return err;
	}
	LOG_INF("tuning saved to flash kp=%.4f ki=%.3f kd=%.4f", (double)g.kp, (double)g.ki,
		(double)g.kd);
	return 0;
}
