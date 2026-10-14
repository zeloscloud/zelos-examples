#include <zelos/can.h>

#include "pdu_node.h"

int main(void)
{
	if (zelos_can_init() == 0) {
		(void)pdu_start();
	}

	return 0;
}
