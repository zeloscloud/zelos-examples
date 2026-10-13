/*
 * Motor controller.
 *
 * A brushed DC motor speed controller whose debug interface is its serial
 * console, the way most boards on a bench are brought up. The log and the
 * shell share the console UART:
 *
 *   motor target <rpm>    the speed target, -6000 to 6000
 *   motor load <Nm>       an external load torque, 0 to 0.15
 *   motor stop | status
 *   pid show | save | defaults
 *   pid kp|ki|kd <value>  tune the speed loop live
 *   fault crash | hang | dump
 *
 * At boot it logs its version, why the board reset, and, if the last run
 * crashed, where. A crash record lives in RAM that survives the reset.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "motor.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define CRASH_MAGIC 0xC0FFEE42u

/* Not cleared at boot: written by the fatal error handler, read after the reset. */
static __noinit struct {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	char thread[16];
} crash;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *name = k_thread_name_get(k_current_get());

	crash.magic = CRASH_MAGIC;
	crash.reason = reason;
	crash.pc = esf != NULL ? esf->basic.pc : 0;
	strncpy(crash.thread, name != NULL ? name : "?", sizeof(crash.thread) - 1);
	crash.thread[sizeof(crash.thread) - 1] = '\0';

	LOG_ERR("rebooting after fatal error %u", reason);
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static const char *reset_cause(uint32_t cause)
{
	/* The most telling cause first: a watchdog reset also sets the pin flag. */
	if (cause & RESET_WATCHDOG) {
		return "watchdog";
	}
	if (cause & RESET_SOFTWARE) {
		return "software";
	}
	if (cause & RESET_BROWNOUT) {
		return "brownout";
	}
	if (cause & RESET_POR) {
		return "power-on";
	}
	if (cause & RESET_PIN) {
		return "reset-pin";
	}
	return "unknown";
}

static void report_boot(void)
{
	uint32_t cause = 0;

	hwinfo_get_reset_cause(&cause);
	hwinfo_clear_reset_cause();

	LOG_INF("motor-ctl v%s board=%s reset=%s", FW_VERSION, CONFIG_BOARD, reset_cause(cause));

	if (crash.magic == CRASH_MAGIC) {
		LOG_ERR("previous run crashed reason=%u pc=0x%08x thread=%s", crash.reason,
			crash.pc, crash.thread);
	} else if (cause & RESET_WATCHDOG) {
		LOG_ERR("previous run stopped feeding the watchdog");
	}
	crash.magic = 0;
}

/* "1500", "-2.5" and "0.05" are numbers; "", "12x" and "1e999" are not. */
static int parse(const struct shell *sh, const char *s, float lo, float hi, float *out)
{
	char *end;
	const float v = strtof(s, &end);

	if (end == s || *end != '\0' || v < lo || v > hi) {
		shell_error(sh, "expected a number from %g to %g", (double)lo, (double)hi);
		return -EINVAL;
	}
	*out = v;
	return 0;
}

static int cmd_target(const struct shell *sh, size_t argc, char **argv)
{
	float rpm;

	if (parse(sh, argv[1], -6000.0f, 6000.0f, &rpm) != 0) {
		return -EINVAL;
	}
	control_set_target(rpm);
	shell_print(sh, "target %.0f rpm", (double)rpm);
	return 0;
}

static int cmd_load(const struct shell *sh, size_t argc, char **argv)
{
	float nm;

	if (parse(sh, argv[1], 0.0f, 0.15f, &nm) != 0) {
		return -EINVAL;
	}
	control_set_load(nm);
	shell_print(sh, "load %.3f Nm", (double)nm);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	control_set_target(0.0f);
	shell_print(sh, "target 0 rpm");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct pid_gains g = control_gains();

	shell_print(sh, "target   %.0f rpm", (double)control_target());
	shell_print(sh, "load     %.3f Nm", (double)control_load());
	shell_print(sh, "limiting %s", control_limiting() ? "yes" : "no");
	shell_print(sh, "winding  %.1f C", (double)control_winding_c());
	shell_print(sh, "gains    kp=%.4f ki=%.3f kd=%.4f", (double)g.kp, (double)g.ki,
		    (double)g.kd);
	return 0;
}

static int cmd_pid_show(const struct shell *sh, size_t argc, char **argv)
{
	const struct pid_gains g = control_gains();

	shell_print(sh, "kp %.4f A/rpm", (double)g.kp);
	shell_print(sh, "ki %.3f A/(rpm s)", (double)g.ki);
	shell_print(sh, "kd %.4f A s/rpm", (double)g.kd);
	return 0;
}

static int cmd_pid_gain(const struct shell *sh, size_t argc, char **argv)
{
	struct pid_gains g = control_gains();
	float *field = strcmp(argv[0], "kp") == 0   ? &g.kp
		       : strcmp(argv[0], "ki") == 0 ? &g.ki
						    : &g.kd;
	float v;

	if (parse(sh, argv[1], 0.0f, 10.0f, &v) != 0) {
		return -EINVAL;
	}
	*field = v;
	control_set_gains(g);
	tuning_report("gains changed");
	shell_print(sh, "%s %g", argv[0], (double)v);
	return 0;
}

static int cmd_pid_save(const struct shell *sh, size_t argc, char **argv)
{
	return tuning_save();
}

static int cmd_pid_defaults(const struct shell *sh, size_t argc, char **argv)
{
	control_set_gains(PID_DEFAULTS);
	tuning_report("gains reset to defaults");
	return 0;
}

static int cmd_fault_crash(const struct shell *sh, size_t argc, char **argv)
{
	LOG_WRN("executing an undefined instruction on purpose");
	k_msleep(20);
	/* A real CPU exception, the kind a corrupted function pointer causes. */
	__asm__ volatile("udf #0");
	return 0;
}

static int cmd_fault_hang(const struct shell *sh, size_t argc, char **argv)
{
	LOG_WRN("stalling the control loop on purpose; the watchdog resets the board in 100 ms");
	k_msleep(20);
	control_stall();
	return 0;
}

static int cmd_fault_dump(const struct shell *sh, size_t argc, char **argv)
{
	const struct pid_gains g = control_gains();

	LOG_HEXDUMP_INF(&g, sizeof(g), "pid gains as stored in flash");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(motor_cmds,
	SHELL_CMD_ARG(target, NULL, "<rpm>  Speed target, -6000 to 6000", cmd_target, 2, 0),
	SHELL_CMD_ARG(load, NULL, "<Nm>  Load torque, 0 to 0.15", cmd_load, 2, 0),
	SHELL_CMD_ARG(stop, NULL, "Target 0 rpm", cmd_stop, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show the controller's state", cmd_status, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(motor, &motor_cmds, "Motor speed control", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(pid_cmds,
	SHELL_CMD_ARG(show, NULL, "Show the speed loop's gains", cmd_pid_show, 1, 0),
	SHELL_CMD_ARG(kp, NULL, "<value>  Proportional gain, A/rpm", cmd_pid_gain, 2, 0),
	SHELL_CMD_ARG(ki, NULL, "<value>  Integral gain, A/(rpm s)", cmd_pid_gain, 2, 0),
	SHELL_CMD_ARG(kd, NULL, "<value>  Derivative gain, A s/rpm", cmd_pid_gain, 2, 0),
	SHELL_CMD_ARG(save, NULL, "Save the gains to flash", cmd_pid_save, 1, 0),
	SHELL_CMD_ARG(defaults, NULL, "Restore the shipped gains", cmd_pid_defaults, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pid, &pid_cmds, "Speed loop tuning", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(fault_cmds,
	SHELL_CMD_ARG(crash, NULL, "Execute an undefined instruction", cmd_fault_crash, 1, 0),
	SHELL_CMD_ARG(hang, NULL, "Stall the control loop until the watchdog fires",
		      cmd_fault_hang, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "Hex-dump the stored gains", cmd_fault_dump, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(fault, &fault_cmds, "Inject faults", NULL);

int main(void)
{
	report_boot();
	tuning_load();
	return 0;
}
