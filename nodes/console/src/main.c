/*
 * Device console.
 *
 * A node whose interface is its serial console. It logs a DC-DC converter's
 * status at 50 Hz and takes shell commands on the same UART:
 *
 *   dcdc limit get | set <A>   the input current limit, 0.0 to 20.0 A
 *   burst <n>                  n lines of output, as fast as the UART allows
 *
 * The input current follows the limit at a capped slew, as the converter in
 * nodes/dcdc does. Everything is a pure function of time and the limit, so two
 * runs print the same lines.
 */

#include <ctype.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(dcdc, LOG_LEVEL_INF);

#define CYCLE_MS 20

/*
 * Fixed point, so no float printf: current in centiamps (ca) or deciamps (da),
 * voltage in centivolts (cv), temperature in decidegrees (dc).
 */
#define LIMIT_DEFAULT_DA 45
#define LIMIT_MAX_DA 200

/* The full 20 A range takes 8 cycles, 160 ms. */
#define SLEW_CA_PER_CYCLE 250

#define RAIL_NOMINAL_CV 1380
#define RAIL_DROOP_CA_PER_CV 40 /* 0.5 V at 20 A */
#define RAIL_RIPPLE_CV 5

/* Triangle wave, 35.0 to 45.0 C, one period a minute. */
#define TEMP_MIN_DC 350
#define TEMP_PERIOD_CYCLES (60 * MSEC_PER_SEC / CYCLE_MS)
#define TEMP_CYCLES_PER_DC (TEMP_PERIOD_CYCLES / 2 / 100)

#define BURST_MAX 100000U

/* Written by the shell thread, read by the main loop. */
static atomic_t limit_da = ATOMIC_INIT(LIMIT_DEFAULT_DA);

/* "2", "2.5" and "20.0" are valid; "2.55", "-1" and "20.1" are not. */
static int parse_deciamps(const char *s, int *da)
{
	char *end;
	unsigned long whole;
	int frac = 0;

	if (!isdigit((unsigned char)s[0])) {
		return -EINVAL;
	}

	whole = strtoul(s, &end, 10);
	if (*end == '.') {
		if (!isdigit((unsigned char)end[1]) || end[2] != '\0') {
			return -EINVAL;
		}
		frac = end[1] - '0';
	} else if (*end != '\0') {
		return -EINVAL;
	}

	if (whole * 10 + frac > LIMIT_MAX_DA) {
		return -ERANGE;
	}

	*da = whole * 10 + frac;

	return 0;
}

static int cmd_limit_get(const struct shell *sh, size_t argc, char **argv)
{
	int da = atomic_get(&limit_da);

	shell_print(sh, "limit: %d.%d A", da / 10, da % 10);

	return 0;
}

static int cmd_limit_set(const struct shell *sh, size_t argc, char **argv)
{
	int da;

	if (parse_deciamps(argv[1], &da) != 0) {
		shell_error(sh, "limit must be 0.0 to 20.0 A, one decimal at most");
		return -EINVAL;
	}

	atomic_set(&limit_da, da);
	shell_print(sh, "limit set to %d.%d A", da / 10, da % 10);

	return 0;
}

static int cmd_burst(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long n = strtoul(argv[1], &end, 10);

	if (*end != '\0' || n < 1 || n > BURST_MAX) {
		shell_error(sh, "n must be 1 to %u", BURST_MAX);
		return -EINVAL;
	}

	/* shell_print blocks until the UART takes the line, so no line is dropped. */
	for (unsigned long i = 0; i < n; i++) {
		shell_print(sh, "seq=%lu", i);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(limit_cmds,
			       SHELL_CMD_ARG(get, NULL, "Show the input current limit",
					     cmd_limit_get, 1, 0),
			       SHELL_CMD_ARG(set, NULL, "<A>  Set the input current limit",
					     cmd_limit_set, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(dcdc_cmds,
			       SHELL_CMD(limit, &limit_cmds, "Input current limit", NULL),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(dcdc, &dcdc_cmds, "DC-DC converter", NULL);
SHELL_CMD_ARG_REGISTER(burst, NULL, "<n>  Print n numbered lines", cmd_burst, 2, 0);

int main(void)
{
	int in_ca = atomic_get(&limit_da) * 10;
	int64_t deadline = k_uptime_get();

	for (uint32_t cycle = 0;; cycle++) {
		const int limit = atomic_get(&limit_da);
		const int phase = cycle % TEMP_PERIOD_CYCLES;
		const int triangle = MIN(phase, TEMP_PERIOD_CYCLES - phase);
		const int ripple_cv = (int)(cycle % (2 * RAIL_RIPPLE_CV + 1)) - RAIL_RIPPLE_CV;

		in_ca += CLAMP(limit * 10 - in_ca, -SLEW_CA_PER_CYCLE, SLEW_CA_PER_CYCLE);

		const int rail_cv = RAIL_NOMINAL_CV - in_ca / RAIL_DROOP_CA_PER_CV + ripple_cv;
		const int temp_dc = TEMP_MIN_DC + triangle / TEMP_CYCLES_PER_DC;

		LOG_INF("rail=%d.%02dV in=%d.%02dA limit=%d.%dA temp=%d.%dC", rail_cv / 100,
			rail_cv % 100, in_ca / 100, in_ca % 100, limit / 10, limit % 10,
			temp_dc / 10, temp_dc % 10);

		deadline += CYCLE_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
