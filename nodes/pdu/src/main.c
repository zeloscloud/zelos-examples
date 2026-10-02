/*
 * Low-voltage power distribution unit, a CANopen CiA 401-style IO node.
 *
 * Switches eight 12 V load channels as a CANopen master commands them (RPDO1
 * or SDO to 0x6200), and reports which channels are powered (0x6000, TPDO1)
 * and what each draws (0x2000, 0.1 A units, TPDO2).
 *
 * A channel whose load draws more than the trip limit is switched off at once
 * and reported by EMCY, which also takes an operational node to
 * pre-operational (0x1029). It stays off while commanded on; commanding it
 * off clears the trip, after which a master can start the node again.
 *
 * While the node has a communication error (its master's heartbeat lost,
 * bus-off), channels with error mode set (0x6206) take their error value
 * (0x6207) instead of what was commanded, as CiA 401 has it.
 */

#include <zelos/canopen.h>

#include <canopennode.h>
/* After the stack, which defines the types it uses. */
#include <CO_OD.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <stdio.h>

LOG_MODULE_REGISTER(pdu, LOG_LEVEL_INF);

#define CHANNELS 8

/* What each channel's load draws when powered, 0.1 A. Channel 8's is shorted. */
static const uint8_t load_da[CHANNELS] = {20, 50, 15, 30, 5, 80, 40, 220};

#define TRIP_DA 150

/* 0x2000, one sub-index per channel. */
static uint8_t *const current_od[CHANNELS] = {
	&OD_channelCurrent.channel1, &OD_channelCurrent.channel2, &OD_channelCurrent.channel3,
	&OD_channelCurrent.channel4, &OD_channelCurrent.channel5, &OD_channelCurrent.channel6,
	&OD_channelCurrent.channel7, &OD_channelCurrent.channel8,
};

/*
 * Runs with the object dictionary locked. TPDOs go out when what they carry
 * changes, which the stack detects itself (TPDODetectCos in the EDS).
 */
static void tick(void)
{
	uint8_t commanded = OD_writeOutput8Bit.output1To8;
	uint8_t powered = 0U;
	uint8_t tripped = 0U;

	if ((OD_errorRegister & CO_ERR_REG_COMM_ERR) != 0U) {
		const uint8_t mode = OD_errorModeOutput8Bit.errorMode1To8;

		commanded = (commanded & ~mode) | (OD_errorValueOutput8Bit.errorValue1To8 & mode);
	}

	for (int ch = 0; ch < CHANNELS; ch++) {
		const uint8_t bit = BIT(ch);
		/*
		 * One error status bit per channel, so trips clear independently.
		 * The bit is the trip: reset-communication clears it, and a channel
		 * still commanded on trips again.
		 */
		const uint8_t error_bit = CO_EM_MANUFACTURER_START + ch;
		bool is_tripped = CO_isError(CO->em, error_bit);

		if ((commanded & bit) == 0U) {
			if (is_tripped) {
				CO_errorReset(CO->em, error_bit, ch + 1);
				LOG_INF("channel %d trip cleared", ch + 1);
				is_tripped = false;
			}
		} else if (!is_tripped) {
			if (load_da[ch] > TRIP_DA) {
				CO_errorReport(CO->em, error_bit, CO_EMC_CURRENT_OUTPUT, ch + 1);
				LOG_WRN("channel %d tripped at %d.%d A", ch + 1, load_da[ch] / 10,
					load_da[ch] % 10);
				is_tripped = true;
			} else {
				powered |= bit;
			}
		}

		tripped |= is_tripped ? bit : 0U;
		*current_od[ch] = (powered & bit) != 0U ? load_da[ch] : 0U;
	}

	/* The stack derives the other error register bits; the current bit is ours. */
	if (tripped != 0U) {
		OD_errorRegister |= CO_ERR_REG_CURRENT;
	} else {
		OD_errorRegister &= (uint8_t)~CO_ERR_REG_CURRENT;
	}

	OD_readInput8Bit.input1To8 = powered;
}

int main(void)
{
	LOG_INF("PDU up, %d channels, trip at %d.%d A", CHANNELS, TRIP_DA / 10, TRIP_DA % 10);
	/* Longer than one SDO frame, so masters read it segmented or by block. */
	snprintf((char *)OD_buildInfo, sizeof(OD_buildInfo), "zelos pdu %s %s %s", CONFIG_BOARD,
		 __DATE__, __TIME__);

	return zelos_canopen_run(tick);
}
