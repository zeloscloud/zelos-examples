/*
 * The speed loop: a PI controller (with an optional D term) at 1 kHz, giving a
 * current reference to an inner current loop, limited to I_LIMIT_A.
 *
 * The motor is a model on the chip: a brushed DC motor on a 24 V bus, its
 * winding inductance, back-EMF, rotor inertia, friction and an external load
 * torque, integrated in ten 100 us sub-steps per control period. The winding
 * heats with I^2R and cools to the die temperature the health thread measures.
 *
 * Logged at 100 Hz from module motor, with the speed error as a Teleplot point.
 * After every change of target, module tune logs the step's rise time,
 * overshoot and settling time, measured on the chip.
 */

#include <math.h>

#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "motor.h"

LOG_MODULE_REGISTER(motor, LOG_LEVEL_INF);

#define PERIOD_US 1000
#define SUBSTEPS 10
#define DT (PERIOD_US * 1e-6f)
#define SUB_DT (DT / SUBSTEPS)
#define TELEMETRY_EVERY 10 /* periods: 100 Hz */

/* The motor: Maxon-class 24 V brushed DC. */
#define R_OHM 0.6f
#define L_H 1.0e-3f
#define K_NM_PER_A 0.035f /* torque constant = back-EMF constant */
#define J_KGM2 4.0e-5f
#define B_NMS 2.0e-6f
#define VBUS 24.0f
#define CURRENT_LOOP_GAIN 40.0f /* V per A of current error */
#define RPM_PER_RAD_S (60.0f / (2.0f * 3.14159265f))

#define I_LIMIT_A 5.0f
#define I_DERATED_A 2.5f
#define WINDING_HOT_C 80.0f
#define WINDING_TRIP_C 100.0f
#define R_TH_K_PER_W 4.0f
#define C_TH_J_PER_K 20.0f

static struct k_spinlock lock;
static struct pid_gains gains = PID_DEFAULTS;
static float target_rpm;
static float load_nm;
static float ambient_c = 25.0f;

/* Read by the health thread. */
static atomic_t limiting;
static atomic_t loop_max_us;
static atomic_t stall;
static float winding_c = 25.0f;

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

void control_set_target(float rpm)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	tune_begin(target_rpm, rpm, k_uptime_get());
	target_rpm = rpm;
	k_spin_unlock(&lock, key);
}

float control_target(void)
{
	return target_rpm;
}

void control_set_load(float nm)
{
	load_nm = nm;
}

float control_load(void)
{
	return load_nm;
}

struct pid_gains control_gains(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct pid_gains g = gains;

	k_spin_unlock(&lock, key);
	return g;
}

void control_set_gains(struct pid_gains g)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	gains = g;
	k_spin_unlock(&lock, key);
}

bool control_limiting(void)
{
	return atomic_get(&limiting) != 0;
}

float control_winding_c(void)
{
	return winding_c;
}

uint32_t control_take_loop_max_us(void)
{
	return (uint32_t)atomic_set(&loop_max_us, 0);
}

void control_set_ambient(float celsius)
{
	ambient_c = celsius;
}

void control_stall(void)
{
	atomic_set(&stall, 1);
}

static void watchdog_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window.min = 0,
		.window.max = 100, /* ms: a hundred missed control periods */
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog not ready");
		return;
	}
	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		LOG_ERR("watchdog setup failed: %d", wdt_channel);
		wdt_channel = -1;
		return;
	}
	LOG_INF("watchdog armed, 100 ms timeout");
}

static void control_thread(void *a, void *b, void *c)
{
	float w = 0.0f;     /* rotor speed, rad/s */
	float i = 0.0f;     /* winding current, A */
	float integ = 0.0f; /* integral term, A */
	float prev_err = 0.0f;
	bool hot = false;
	bool tripped = false;
	uint32_t saturated_ms = 0;
	uint32_t free_ms = 0;
	bool limit_reported = false;
	uint64_t next = k_uptime_ticks();

	watchdog_start();

	for (uint32_t n = 0;; n++) {
		const uint32_t t0 = k_cycle_get_32();

		if (atomic_get(&stall)) {
			/* What a deadlocked driver looks like: the loop stops feeding the watchdog. */
			for (;;) {
				k_busy_wait(1000);
			}
		}

		k_spinlock_key_t key = k_spin_lock(&lock);
		const struct pid_gains g = gains;
		const float target = target_rpm;
		k_spin_unlock(&lock, key);

		const float rpm = w * RPM_PER_RAD_S;
		const float err = target - rpm;
		const float deriv = (err - prev_err) / DT;
		const float limit = tripped ? I_DERATED_A : I_LIMIT_A;

		prev_err = err;

		const float iref_free = g.kp * err + integ + g.kd * deriv;
		const float iref = CLAMP(iref_free, -limit, limit);
		const bool saturated = iref != iref_free;

		/* Anti-windup: hold the integrator while the output is limited. */
		if (!saturated) {
			integ += g.ki * err * DT;
		}

		for (int s = 0; s < SUBSTEPS; s++) {
			const float v = CLAMP(R_OHM * iref + K_NM_PER_A * w +
						      CURRENT_LOOP_GAIN * (iref - i),
					      -VBUS, VBUS);

			i += (v - R_OHM * i - K_NM_PER_A * w) / L_H * SUB_DT;
			w += (K_NM_PER_A * i - B_NMS * w - load_nm) / J_KGM2 * SUB_DT;
		}

		/* The winding heats with I^2R and cools to the board's own temperature. */
		winding_c += (i * i * R_OHM - (winding_c - ambient_c) / R_TH_K_PER_W) /
			     C_TH_J_PER_K * DT;

		const int64_t now = k_uptime_get();
		const float duty = 100.0f * (R_OHM * i + K_NM_PER_A * w) / VBUS;

		tune_sample(w * RPM_PER_RAD_S, now);

		if (saturated) {
			saturated_ms++;
			free_ms = 0;
		} else {
			free_ms++;
			saturated_ms = 0;
		}
		if (saturated_ms == 50 && !limit_reported) {
			LOG_WRN("current limit demand=%.2fA limit=%.1fA", (double)iref_free,
				(double)limit);
			limit_reported = true;
		}
		if (free_ms == 100) {
			limit_reported = false;
		}
		atomic_set(&limiting, saturated_ms >= 50);

		if (!hot && winding_c > WINDING_HOT_C) {
			LOG_WRN("winding hot winding=%.1fC", (double)winding_c);
			hot = true;
		} else if (hot && winding_c < WINDING_HOT_C - 5.0f) {
			hot = false;
		}
		if (!tripped && winding_c > WINDING_TRIP_C) {
			LOG_ERR("winding over temperature, derating winding=%.1fC limit=%.1fA",
				(double)winding_c, (double)I_DERATED_A);
			tripped = true;
		} else if (tripped && winding_c < WINDING_TRIP_C - 10.0f) {
			LOG_INF("winding cooled, full current winding=%.1fC limit=%.1fA",
				(double)winding_c, (double)I_LIMIT_A);
			tripped = false;
		}

		if (n % TELEMETRY_EVERY == 0) {
			LOG_INF("speed=%.0frpm target=%.0frpm current=%.2fA duty=%.1f%% winding=%.1fC",
				(double)(w * RPM_PER_RAD_S), (double)target, (double)i,
				(double)duty, (double)winding_c);
			printk(">error:%.1f\xc2\xa7rpm\n", (double)err);
		}

		if (wdt_channel >= 0) {
			wdt_feed(wdt, wdt_channel);
		}

		const uint32_t used_us = k_cyc_to_us_ceil32(k_cycle_get_32() - t0);

		/*
		 * Not one atomic step: the health thread may zero the maximum between
		 * the read and the write. It reads once a second, so at worst one
		 * period's time lands in the next report.
		 */
		if ((atomic_val_t)used_us > atomic_get(&loop_max_us)) {
			atomic_set(&loop_max_us, used_us);
		}
		if (used_us > PERIOD_US / 2) {
			LOG_WRN("control loop overrun used=%uus budget=%uus", used_us, PERIOD_US / 2);
		}

		next += k_us_to_ticks_ceil64(PERIOD_US);
		k_sleep(K_TIMEOUT_ABS_TICKS(next));
	}
}

K_THREAD_DEFINE(control_tid, 2048, control_thread, NULL, NULL, NULL, 2, K_FP_REGS, 1000);
