/* The DC-DC's SOME/IP service, with lib/someip. */

#ifndef DCDC_SOMEIP_H_
#define DCDC_SOMEIP_H_

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

int dcdc_someip_start(void);

/* This cycle's state, with the DCDC_Status Counter of the same sample. */
void dcdc_someip_sample(const struct dcdc_state *state, bool derating, uint8_t counter);

#endif /* DCDC_SOMEIP_H_ */
