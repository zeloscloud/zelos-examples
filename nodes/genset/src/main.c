#include <zelos/can.h>

#include "genset_node.h"

int main(void)
{
	if (zelos_can_init() == 0) {
		(void)genset_start();
	}

	return 0;
}
