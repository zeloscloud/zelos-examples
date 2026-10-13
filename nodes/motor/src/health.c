/*
 * The chip's own state, once a second from module sys: CPU load from the
 * scheduler's cycle accounting, the control loop's worst execution time, and
 * the die temperature and VDDA from the on-chip sensors. The die temperature
 * is the motor model's ambient.
 *
 * LEDs: green blinks while the firmware runs, yellow lights while the current
 * limit holds, red while the winding is hot. The user button steps the target
 * through 0, 1500 and 3000 rpm.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "motor.h"

LOG_MODULE_REGISTER(sys, LOG_LEVEL_INF);

#define TICK_MS 100
#define REPORT_EVERY (MSEC_PER_SEC / TICK_MS)

static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec yellow = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
/* The NUCLEO-H753ZI has its red LED only as a PWM LED, so it may be absent. */
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led2), gpios, {0});
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static const struct device *const die = DEVICE_DT_GET(DT_NODELABEL(die_temp));
static const struct device *const vref = DEVICE_DT_GET(DT_NODELABEL(vref));

static const float button_targets[] = {1500.0f, 3000.0f, 0.0f};
static struct gpio_callback button_cb;
static struct k_work button_work;
static uint32_t presses;

static void button_pressed(struct k_work *work)
{
	const float target = button_targets[presses % ARRAY_SIZE(button_targets)];

	presses++;
	LOG_INF("button press=%u target=%.0frpm", presses, (double)target);
	control_set_target(target);
}

/* No debounce in software: the NUCLEO's user button has an RC filter on the board. */
static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&button_work);
}

static int read_sensor(const struct device *dev, enum sensor_channel chan, float *out)
{
	struct sensor_value v;

	if (!device_is_ready(dev) || sensor_sample_fetch(dev) != 0 ||
	    sensor_channel_get(dev, chan, &v) != 0) {
		return -EIO;
	}
	*out = (float)sensor_value_to_double(&v);
	return 0;
}

static void io_init(void)
{
	gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&yellow, GPIO_OUTPUT_INACTIVE);
	if (red.port != NULL) {
		gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
	}

	k_work_init(&button_work, button_pressed);
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);
}

static void health_thread(void *a, void *b, void *c)
{
	k_thread_runtime_stats_t prev, now;
	bool sensors_warned = false;

	io_init();
	k_thread_runtime_stats_all_get(&prev);

	for (uint32_t n = 1;; n++) {
		k_msleep(TICK_MS);

		gpio_pin_set_dt(&green, (n / 5) % 2);
		gpio_pin_set_dt(&yellow, control_limiting());
		if (red.port != NULL) {
			gpio_pin_set_dt(&red, control_winding_c() > 80.0f);
		}

		if (n % REPORT_EVERY != 0) {
			continue;
		}

		k_thread_runtime_stats_all_get(&now);
		const uint64_t total = now.execution_cycles - prev.execution_cycles;
		const uint64_t idle = now.idle_cycles - prev.idle_cycles;
		const float cpu = total ? 100.0f * (float)(total - idle) / (float)total : 0.0f;

		prev = now;

		float die_c, vdda;

		if (read_sensor(die, SENSOR_CHAN_DIE_TEMP, &die_c) != 0 ||
		    read_sensor(vref, SENSOR_CHAN_VOLTAGE, &vdda) != 0) {
			if (!sensors_warned) {
				LOG_WRN("on-chip sensors unavailable");
				sensors_warned = true;
			}
			LOG_INF("cpu=%.1f%% loop_max=%uus uptime=%llds", (double)cpu,
				control_take_loop_max_us(), k_uptime_get() / MSEC_PER_SEC);
			continue;
		}
		control_set_ambient(die_c);
		LOG_INF("cpu=%.1f%% loop_max=%uus die=%.1fC vdda=%.3fV uptime=%llds", (double)cpu,
			control_take_loop_max_us(), (double)die_c, (double)vdda,
			k_uptime_get() / MSEC_PER_SEC);
	}
}

K_THREAD_DEFINE(health_tid, 2048, health_thread, NULL, NULL, NULL, 5, K_FP_REGS, 500);
