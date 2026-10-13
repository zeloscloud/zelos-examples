/*
 * Step-response metrics: after every change of target, the rise time (to 90 %
 * of the step), the overshoot (past the target, as a share of the step) and
 * the settling time (until the speed stays within 2 % of the step for 200 ms),
 * measured from the control loop's own samples and logged from module tune.
 */

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "motor.h"

LOG_MODULE_REGISTER(tune, LOG_LEVEL_INF);

#define SETTLE_BAND 0.02f
#define SETTLE_HOLD_MS 200
#define TIMEOUT_MS 3000

static struct k_spinlock lock;

static struct {
	bool active;
	float from;
	float to;
	int64_t start_ms;
	float peak_excess;   /* furthest past the target, in rpm */
	int64_t rise_ms;     /* when the speed first reached 90 % of the step, or -1 */
	int64_t last_out_ms; /* the last sample outside the band */
} step;

void tune_begin(float from_rpm, float to_rpm, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	step.active = to_rpm != from_rpm;
	step.from = from_rpm;
	step.to = to_rpm;
	step.start_ms = now_ms;
	step.peak_excess = 0.0f;
	step.rise_ms = -1;
	step.last_out_ms = now_ms;
	k_spin_unlock(&lock, key);
}

void tune_sample(float rpm, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (!step.active) {
		k_spin_unlock(&lock, key);
		return;
	}

	const float size = step.to - step.from;
	/* Signed, so a step down counts its undershoot as overshoot. */
	const float progress = (rpm - step.from) / size;
	const float excess = (rpm - step.to) * (size > 0.0f ? 1.0f : -1.0f);

	if (step.rise_ms < 0 && progress >= 0.9f) {
		step.rise_ms = now_ms - step.start_ms;
	}
	step.peak_excess = MAX(step.peak_excess, excess);
	if (fabsf(rpm - step.to) > SETTLE_BAND * fabsf(size)) {
		step.last_out_ms = now_ms;
	}

	const bool settled = now_ms - step.last_out_ms >= SETTLE_HOLD_MS;
	const bool timed_out = now_ms - step.start_ms >= TIMEOUT_MS;

	if (!settled && !timed_out) {
		k_spin_unlock(&lock, key);
		return;
	}

	/* Copied out, so nothing logs under the lock. */
	const float from = step.from;
	const float to = step.to;
	const float overshoot = 100.0f * step.peak_excess / fabsf(size);
	const int64_t rise = step.rise_ms;
	const int64_t settle = step.last_out_ms - step.start_ms;

	step.active = false;
	k_spin_unlock(&lock, key);

	if (!settled) {
		LOG_WRN("step from=%.0frpm to=%.0frpm did not settle within %dms", (double)from,
			(double)to, TIMEOUT_MS);
		return;
	}
	LOG_INF("step from=%.0frpm to=%.0frpm rise=%lldms overshoot=%.1f%% settle=%lldms",
		(double)from, (double)to, rise, (double)overshoot, settle);
}
