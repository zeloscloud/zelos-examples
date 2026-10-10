/*
 * One MCU hosting three logical ECUs on one controller: the DC-DC (with XCP
 * and UDS), the genset (J1939) and the PDU (CANopen). lib/can brings the bus
 * up once; each node then starts its own threads.
 *
 * The UDS server is the DC-DC's: CommunicationControl and ControlDTCSetting
 * act on the DC-DC alone. ECUReset reboots the MCU, so every node restarts.
 */

#include <zelos/can.h>

#include <zephyr/logging/log.h>

#include "../../dcdc/src/dcdc_node.h"
#include "../../genset/src/genset_node.h"
#include "../../pdu/src/pdu_node.h"

LOG_MODULE_REGISTER(full, LOG_LEVEL_INF);

int main(void)
{
	if (zelos_can_init() != 0) {
		return 0;
	}

	/* Each logs its own failure; the others still start. */
	(void)dcdc_start();
	(void)genset_start();
	(void)pdu_start();

	return 0;
}
