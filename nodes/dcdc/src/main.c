#include <zelos/can.h>

#include "dcdc_node.h"

int main(void)
{
	if (zelos_can_init() == 0) {
		(void)dcdc_start();
	}

	return 0;
}
