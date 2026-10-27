#ifndef DCDC_NODE_H_
#define DCDC_NODE_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Subscribe, start XCP and UDS if built in, and start the control loop.
 * Needs zelos_can_init() first.
 *
 * @return 0, or negative errno.
 */
int dcdc_start(void);

#if defined(CONFIG_ZELOS_GOOSE)
/* The latest cycle, as DCDC_Status encodes it, and cycles run since start. */
struct dcdc_sample {
	float output_v;
	float output_a;
	float input_a;
	float temperature_c;
	bool derating;
	uint32_t cycles;
};

/* From any thread. */
void dcdc_latest(struct dcdc_sample *out);
#endif

#endif /* DCDC_NODE_H_ */
