/*
 * One MCU hosting three logical ECUs on one controller: the DC-DC (with XCP,
 * UDS and AUTOSAR PDUs), the genset (J1939) and the PDU (CANopen). lib/can
 * brings the bus up once; each node then starts its own threads. A signal
 * generator (nodes/full/src/waveform.c) adds a chirped Waveform frame for plots.
 * Ethernet (lib/eth) comes up beside them, at 10.1.1.75 by default, with
 * GOOSE (lib/goose, nodes/full/src/goose.c) on it.
 *
 * The UDS server is the DC-DC's: CommunicationControl and ControlDTCSetting
 * act on the DC-DC alone. ECUReset reboots the MCU, so every node restarts.
 */

#include <zelos/can.h>
#include <zelos/eth.h>

#include <zephyr/logging/log.h>

#include "../../dcdc/src/dcdc_node.h"
#include "../../genset/src/genset_node.h"
#include "../../pdu/src/pdu_node.h"
#include "goose.h"
#include "waveform.h"

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
	(void)waveform_start();
#if defined(CONFIG_ZELOS_ETH)
	/* Last, and it never waits: CAN starts as it would with no cable. */
	(void)zelos_eth_start();
#endif
#if defined(CONFIG_ZELOS_GOOSE)
	(void)full_goose_start();
#endif

	return 0;
}
