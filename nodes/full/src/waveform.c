/*
 * A signal generator for plots: Waveform (0x7F0) at 500 Hz carrying sine,
 * square and sawtooth of one phase, the frequency swept 1 -> 100 Hz over 20 s
 * and wrapped. The lowest-priority ID on the bus, so it yields to every node.
 * No libm: the sine table is built once by rotating a unit vector.
 */

#include <bench.h>
#include <zelos/can.h>

#include "waveform.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(waveform, LOG_LEVEL_INF);

#define PERIOD_MS   BENCH_WAVEFORM_CYCLE_TIME_MS
#define F0_HZ       1.0f
#define F1_HZ       100.0f
#define SWEEP_MS    20000
#define TABLE_BITS  8
#define TABLE_SIZE  (1 << TABLE_BITS)

static float sine_table[TABLE_SIZE];

static void build_table(void)
{
	/* cos and sin of 2*pi/256; the recurrence drifts ~1e-5 over one turn. */
	const float c = 0.99969881869620f;
	const float s = 0.02454122852291f;
	float x = 1.0f;
	float y = 0.0f;

	for (int i = 0; i < TABLE_SIZE; i++) {
		sine_table[i] = y;
		float nx = x * c - y * s;
		y = x * s + y * c;
		x = nx;
	}
}

static atomic_t in_flight;

static void left(struct zelos_can_done *done, int error)
{
	ARG_UNUSED(done);
	ARG_UNUSED(error);
	atomic_clear(&in_flight);
}

static struct zelos_can_done sent = {.fn = left};

static void run(void *p1, void *p2, void *p3)
{
	const float dt_s = (float)PERIOD_MS / 1000.0f;
	uint32_t phase = 0U; /* one turn = 2^32 */
	int64_t deadline;
	int64_t t0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	build_table();
	deadline = t0 = k_uptime_get();

	for (;;) {
		struct bench_waveform_t msg = {0};
		uint8_t buf[BENCH_WAVEFORM_LENGTH];
		const int64_t t_ms = (deadline - t0) % SWEEP_MS;
		const float f_hz = F0_HZ + (F1_HZ - F0_HZ) * (float)t_ms / (float)SWEEP_MS;
		const float frac = (float)phase / 4294967296.0f;
		const float sine = sine_table[phase >> (32 - TABLE_BITS)];

		/* Raw units (0.01 Hz, 0.001), rounded: the encoders truncate. */
		msg.freq_hz = (uint16_t)(f_hz * 100.0f + 0.5f);
		msg.sine = (int16_t)(sine * 1000.0f + (sine < 0.0f ? -0.5f : 0.5f));
		msg.square = frac < 0.5f ? 1000 : -1000;
		msg.sawtooth = (int16_t)(2000.0f * frac - 1000.0f);

		/* One frame in flight at most, else under load Waveform fills the
		 * controller's TX buffer and higher-priority frames wait on it.
		 * A busy bus costs samples, not the other nodes' periods. */
		if (atomic_cas(&in_flight, 0, 1)) {
			if (bench_waveform_pack(buf, &msg, sizeof(buf)) == (int)sizeof(buf)) {
				(void)zelos_can_send_done(BENCH_WAVEFORM_FRAME_ID, buf, sizeof(buf), &sent);
			} else {
				atomic_clear(&in_flight);
			}
		}

		phase += (uint32_t)(f_hz * dt_s * 4294967296.0f);
		deadline += PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}

K_THREAD_DEFINE(waveform_tid, 1024, run, NULL, NULL, NULL, 2, 0, SYS_FOREVER_MS);

int waveform_start(void)
{
	k_thread_start(waveform_tid);
	LOG_INF("Waveform 0x%03x every %u ms, %d-%d Hz chirp", BENCH_WAVEFORM_FRAME_ID,
		PERIOD_MS, (int)F0_HZ, (int)F1_HZ);
	return 0;
}
