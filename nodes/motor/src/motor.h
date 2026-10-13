/*
 * A brushed DC motor speed controller. The motor itself is simulated on the
 * chip, since none is wired to the board; everything around it is what a real
 * controller does: a 1 kHz control loop with a measured period, a current
 * limit, thermal derating, a watchdog, tuning saved to flash, and crash
 * forensics that survive a reset.
 */

#ifndef MOTOR_H_
#define MOTOR_H_

#include <stdbool.h>
#include <stdint.h>

#define FW_VERSION "1.4.2"

struct pid_gains {
	float kp; /* A per rpm of speed error */
	float ki; /* A per rpm-second */
	float kd; /* A per rpm-per-second */
};

/* As a fresh unit ships: stable, but it rings. */
#define PID_DEFAULTS ((struct pid_gains){.kp = 0.002f, .ki = 0.1f, .kd = 0.0f})

/* control.c: the 1 kHz loop and the simulated motor. */
void control_set_target(float rpm);
float control_target(void);
void control_set_load(float nm);
float control_load(void);
struct pid_gains control_gains(void);
void control_set_gains(struct pid_gains gains);
bool control_limiting(void);
float control_winding_c(void);
uint32_t control_take_loop_max_us(void);
void control_set_ambient(float celsius);
void control_stall(void);

/* tune.c: step-response metrics, measured on the chip. */
void tune_begin(float from_rpm, float to_rpm, int64_t now_ms);
void tune_sample(float rpm, int64_t now_ms);

/* settings.c: the tuning in flash. */
int tuning_load(void);
int tuning_save(void);
void tuning_report(const char *what);

#endif /* MOTOR_H_ */
