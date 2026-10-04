/*
 * Low-voltage power distribution unit, a CANopen CiA 401-style IO node.
 *
 * Switches eight 12 V load channels as a CANopen master commands them (RPDO1,
 * RPDO2 or SDO to 0x6200), and reports which channels are powered (0x6000,
 * TPDO1) and what each draws (0x2000, 0.1 A units, TPDO2). On every SYNC,
 * TPDO3 sends the inputs with a millisecond counter (0x2003). The last TIME
 * message received is kept in 0x2004.
 *
 * A channel whose load draws more than the trip limit is switched off at once
 * and reported by EMCY, which also takes an operational node to
 * pre-operational (0x1029). The trip latches: the channel stays off until a
 * master commands it off in 0x6200, or resets communication. Then it can be
 * commanded on again and the node started.
 *
 * While the error register's communication bit (0x1001 bit 4) is set,
 * channels with error mode set (0x6206) take their error value (0x6207)
 * instead of what was commanded, as CiA 401 has it. A communication error
 * never clears a trip. The stack sets that bit for:
 * - CAN bus-off, or a transmit overflow, until the bus recovers;
 * - a heartbeat consumed in 0x1016 lost, or its node rebooting, until that
 *   heartbeat returns;
 * - a SYNC of the wrong length, a SYNC timeout (only with 0x1006 set), a TPDO
 *   dropped outside the SYNC window, or a PDO mapping error, all held until
 *   reset-communication.
 * Bus warning, bus passive and a wrong-length RPDO do not set it.
 */

#include <zelos/canopen.h>

#include <canopennode.h>
/* After the stack, which defines the types it uses. */
#include <CO_OD.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>
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
	const uint8_t commanded = OD_writeOutput8Bit.output1To8;
	uint8_t outputs = commanded;
	uint8_t powered = 0U;
	uint8_t tripped = 0U;

	if ((OD_errorRegister & CO_ERR_REG_COMM_ERR) != 0U) {
		const uint8_t mode = OD_errorModeOutput8Bit.errorMode1To8;

		outputs = (commanded & ~mode) | (OD_errorValueOutput8Bit.errorValue1To8 & mode);
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

		/* Only the master clears a trip, and not while an error value holds the channel on. */
		if (is_tripped && ((commanded | outputs) & bit) == 0U) {
			CO_errorReset(CO->em, error_bit, ch + 1);
			LOG_INF("channel %d trip cleared", ch + 1);
			is_tripped = false;
		}

		if ((outputs & bit) != 0U && !is_tripped) {
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

	/* Free-running, so TPDO3 shows when the node sampled each SYNC. */
	OD_millisecondCounter = (uint16_t)k_uptime_get_32();

	/* The stack writes the last TIME message from the CAN receive interrupt. */
	const unsigned int key = irq_lock();
	const uint64_t time_of_day = CO->TIME->Time.ullValue;

	irq_unlock(key);
	OD_lastTIMEReceived.millisecondsAfterMidnight = (uint32_t)time_of_day & 0x0FFFFFFFU;
	OD_lastTIMEReceived.daysSince19840101 = (uint16_t)(time_of_day >> 32);
}

int main(void)
{
	LOG_INF("PDU up, %d channels, trip at %d.%d A", CHANNELS, TRIP_DA / 10, TRIP_DA % 10);
	/* Longer than one SDO frame, so masters read it segmented or by block. */
	snprintf((char *)OD_buildInfo, sizeof(OD_buildInfo), "zelos pdu %s zephyr %s", CONFIG_BOARD,
		 KERNEL_VERSION_STRING);

	return zelos_canopen_run(tick);
}
