/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified by Zelos Cloud, Inc. (2026) to share a controller lib/can owns.
 */

/*
 * CANopenNode's driver on a controller it shares.
 *
 * Replaces canopennodezephyr's src/CO_driver.c (see CMakeLists.txt), which
 * owns the controller: it stops it, sets its bitrate and restarts it on every
 * CO_init, so each NMT reset-communication took the bus from every other
 * protocol on the node. Here lib/can owns the controller and this never
 * touches its mode or timing. Frames go out through lib/can's queue; each
 * receive buffer gets its own filter, demultiplexed in software. Error state
 * is read from the controller as the module's driver does.
 */

#include <canopennode.h>
#include <zelos/can.h>

#include <limits.h>
#include <string.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zelos_co_driver, CONFIG_CANOPENNODE_LOG_LEVEL);

static K_MUTEX_DEFINE(send_mutex);
static K_MUTEX_DEFINE(emcy_mutex);
static K_MUTEX_DEFINE(od_mutex);

static canopen_rxmsg_callback_t rxmsg_callback;

/* The one module CO_init builds. The receive ISR finds it from here. */
static CO_CANmodule_t *module;

void canopen_send_lock(void)
{
	k_mutex_lock(&send_mutex, K_FOREVER);
}

void canopen_send_unlock(void)
{
	k_mutex_unlock(&send_mutex);
}

void canopen_emcy_lock(void)
{
	k_mutex_lock(&emcy_mutex, K_FOREVER);
}

void canopen_emcy_unlock(void)
{
	k_mutex_unlock(&emcy_mutex);
}

void canopen_od_lock(void)
{
	k_mutex_lock(&od_mutex, K_FOREVER);
}

void canopen_od_unlock(void)
{
	k_mutex_unlock(&od_mutex);
}

void canopen_set_rxmsg_callback(canopen_rxmsg_callback_t callback)
{
	rxmsg_callback = callback;
}

/*
 * One frame can match several of this module's filters: SYNC 0x080 lies in
 * the EMCY consumer's 0x080/0x780. M_CAN reports only the first matching
 * filter; other controllers report every match. Acting only from the
 * lowest-numbered matching filter handles each frame once on either. The
 * buffer that takes it is the first match in the stack's order, as the
 * module's driver has it, so SYNC goes to SYNC.
 */
static void rx_isr(const struct device *dev, struct can_frame *frame, void *user_data)
{
	const CO_CANrx_t *fired = user_data;
	CO_CANrx_t *target = NULL;
	int first_filter = INT_MAX;
	CO_CANrxMsg_t msg;

	ARG_UNUSED(dev);

	/* The stack takes no remote frames; see CO_CANrxBufferInit. */
	if ((frame->flags & CAN_FRAME_RTR) != 0U) {
		return;
	}

	for (uint16_t i = 0U; i < module->rx_size; i++) {
		CO_CANrx_t *buffer = &module->rx_array[i];

		if (buffer->filter_id < 0 || ((frame->id ^ buffer->ident) & buffer->mask) != 0U) {
			continue;
		}
		if (target == NULL) {
			target = buffer;
		}
		first_filter = MIN(first_filter, buffer->filter_id);
	}

	if (target == NULL || fired->filter_id != first_filter) {
		return;
	}

	msg.ident = frame->id;
	msg.DLC = MIN(can_dlc_to_bytes(frame->dlc), sizeof(msg.data));
	memcpy(msg.data, frame->data, msg.DLC);
	target->pFunct(target->object, &msg);

	if (rxmsg_callback != NULL) {
		rxmsg_callback();
	}
}

static void remove_filter(CO_CANmodule_t *CANmodule, CO_CANrx_t *buffer)
{
	if (buffer->filter_id >= 0) {
		can_remove_rx_filter(CANmodule->dev, buffer->filter_id);
		buffer->filter_id = -ENOSPC;
	}
}

/* Into lib/can's queue, never waiting: false if the queue is full. */
static bool try_queue(const CO_CANtx_t *buffer)
{
	struct can_frame frame = {
		.id = buffer->ident,
		.dlc = buffer->DLC,
		.flags = buffer->rtr ? CAN_FRAME_RTR : 0U,
	};

	memcpy(frame.data, buffer->data, buffer->DLC);

	return zelos_can_try_queue(&frame) == 0;
}

/*
 * A frame the queue refused waits in its buffer (bufferFull), as one the
 * controller refused does with the module's driver: SDO and EMCY hold off
 * while it does, and a second send of that buffer reports CAN TX overflow.
 * CO_CANverifyErrors retries them on every CO_process.
 */
static void retry_waiting(CO_CANmodule_t *CANmodule)
{
	CO_LOCK_CAN_SEND();
	for (uint16_t i = 0U; i < CANmodule->tx_size; i++) {
		CO_CANtx_t *buffer = &CANmodule->tx_array[i];

		if (!buffer->bufferFull) {
			continue;
		}
		if (!try_queue(buffer)) {
			break;
		}
		buffer->bufferFull = false;
	}
	CO_UNLOCK_CAN_SEND();
}

/* lib/can started the controller, and nothing here stops it. */
void CO_CANsetConfigurationMode(void *CANdriverState)
{
	ARG_UNUSED(CANdriverState);
}

void CO_CANsetNormalMode(CO_CANmodule_t *CANmodule)
{
	CANmodule->CANnormal = true;
}

/* CANbitRate is unused: lib/can set the bitrate for every protocol. */
CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t *CANmodule, void *CANdriverState,
				   CO_CANrx_t rxArray[], uint16_t rxSize, CO_CANtx_t txArray[],
				   uint16_t txSize, uint16_t CANbitRate)
{
	struct canopen_context *ctx = CANdriverState;

	ARG_UNUSED(CANbitRate);

	if (CANmodule == NULL || rxArray == NULL || txArray == NULL || ctx == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	/* Reset-communication comes back here: the last init's filters go first. */
	if (CANmodule->configured) {
		CO_CANmodule_disable(CANmodule);
	}

	CANmodule->dev = ctx->dev;
	CANmodule->rx_array = rxArray;
	CANmodule->rx_size = rxSize;
	CANmodule->tx_array = txArray;
	CANmodule->tx_size = txSize;
	CANmodule->CANnormal = false;
	CANmodule->first_tx_msg = true;
	CANmodule->errors = 0U;
	CANmodule->em = NULL;

	for (uint16_t i = 0U; i < rxSize; i++) {
		rxArray[i].ident = 0U;
		rxArray[i].pFunct = NULL;
		rxArray[i].filter_id = -ENOSPC;
	}

	for (uint16_t i = 0U; i < txSize; i++) {
		txArray[i].bufferFull = false;
	}

	module = CANmodule;
	CANmodule->configured = true;

	return CO_ERROR_NO;
}

/* Frees this module's filters; the controller keeps running. */
void CO_CANmodule_disable(CO_CANmodule_t *CANmodule)
{
	if (CANmodule == NULL || CANmodule->rx_array == NULL) {
		return;
	}

	for (uint16_t i = 0U; i < CANmodule->rx_size; i++) {
		remove_filter(CANmodule, &CANmodule->rx_array[i]);
	}
}

uint16_t CO_CANrxMsg_readIdent(const CO_CANrxMsg_t *rxMsg)
{
	return rxMsg->ident;
}

CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
				    uint16_t mask, bool_t rtr, void *object,
				    CO_CANrxBufferCallback_t pFunct)
{
	struct can_filter filter = {.id = ident, .mask = mask};
	CO_CANrx_t *buffer;

	if (CANmodule == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	/* The stack registers no remote-frame reception; refuse rather than half-support it. */
	if (pFunct == NULL || index >= CANmodule->rx_size || rtr) {
		LOG_ERR("rx buffer %u: illegal argument", index);
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	buffer = &CANmodule->rx_array[index];
	/* Off before the buffer changes, so the ISR never sees it half-written. */
	remove_filter(CANmodule, buffer);
	buffer->object = object;
	buffer->pFunct = pFunct;
	buffer->ident = ident;
	buffer->mask = mask;

	buffer->filter_id = can_add_rx_filter(CANmodule->dev, rx_isr, buffer, &filter);
	if (buffer->filter_id < 0) {
		LOG_ERR("no filter for 0x%03x: %d", ident, buffer->filter_id);
		CO_errorReport(CANmodule->em, CO_EM_MEMORY_ALLOCATION_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return CO_ERROR_OUT_OF_MEMORY;
	}

	return CO_ERROR_NO;
}

CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *CANmodule, uint16_t index, uint16_t ident,
			       bool_t rtr, uint8_t noOfBytes, bool_t syncFlag)
{
	CO_CANtx_t *buffer;

	if (CANmodule == NULL) {
		return NULL;
	}

	if (index >= CANmodule->tx_size) {
		LOG_ERR("tx buffer %u: illegal argument", index);
		CO_errorReport(CANmodule->em, CO_EM_GENERIC_SOFTWARE_ERROR,
			       CO_EMC_SOFTWARE_INTERNAL, 0);
		return NULL;
	}

	buffer = &CANmodule->tx_array[index];
	buffer->ident = ident;
	buffer->rtr = rtr;
	buffer->DLC = noOfBytes;
	buffer->bufferFull = false;
	buffer->syncFlag = syncFlag;

	return buffer;
}

CO_ReturnError_t CO_CANsend(CO_CANmodule_t *CANmodule, CO_CANtx_t *buffer)
{
	CO_ReturnError_t ret = CO_ERROR_NO;

	if (CANmodule == NULL || buffer == NULL) {
		return CO_ERROR_ILLEGAL_ARGUMENT;
	}

	CO_LOCK_CAN_SEND();
	/* The last frame from this buffer never left: it is overwritten. */
	if (buffer->bufferFull) {
		zelos_can_dropped();
		if (!CANmodule->first_tx_msg) {
			CO_errorReport(CANmodule->em, CO_EM_CAN_TX_OVERFLOW, CO_EMC_CAN_OVERRUN,
				       buffer->ident);
		}
		ret = CO_ERROR_TX_OVERFLOW;
	}
	buffer->bufferFull = !try_queue(buffer);
	CO_UNLOCK_CAN_SEND();

	return ret;
}

/*
 * Synchronous TPDOs still waiting in their buffers when the SYNC window
 * closes are dropped. Ones already in lib/can's queue go out late instead.
 */
void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *CANmodule)
{
	bool_t dropped = false;

	CO_LOCK_CAN_SEND();
	for (uint16_t i = 0U; i < CANmodule->tx_size; i++) {
		CO_CANtx_t *buffer = &CANmodule->tx_array[i];

		if (buffer->bufferFull && buffer->syncFlag) {
			zelos_can_dropped();
			buffer->bufferFull = false;
			dropped = true;
		}
	}
	CO_UNLOCK_CAN_SEND();

	if (dropped) {
		CO_errorReport(CANmodule->em, CO_EM_TPDO_OUTSIDE_WINDOW, CO_EMC_COMMUNICATION, 0);
	}
}

/* Called from CO_process. The bus checks are the module's driver's. */
void CO_CANverifyErrors(CO_CANmodule_t *CANmodule)
{
	CO_EM_t *em = (CO_EM_t *)CANmodule->em;
	struct can_bus_err_cnt err_cnt;
	enum can_state state;
	uint32_t errors;
	int err;

	retry_waiting(CANmodule);

	/* Until the bus acknowledges a frame, a lone node's errors are expected. */
	if (CANmodule->first_tx_msg && zelos_can_stats().sent > 0U) {
		CANmodule->first_tx_msg = false;
	}

	err = can_get_state(CANmodule->dev, &state, &err_cnt);
	if (err != 0) {
		LOG_ERR("can_get_state: %d", err);
		return;
	}

	/* Zephyr has no API for the RX overflow count the low byte would carry. */
	errors = ((uint32_t)err_cnt.tx_err_cnt << 16) | ((uint32_t)err_cnt.rx_err_cnt << 8);
	if (errors == CANmodule->errors) {
		return;
	}
	CANmodule->errors = errors;

	if (state == CAN_STATE_BUS_OFF) {
		CO_errorReport(em, CO_EM_CAN_TX_BUS_OFF, CO_EMC_BUS_OFF_RECOVERED, errors);
		return;
	}

	CO_errorReset(em, CO_EM_CAN_TX_BUS_OFF, errors);

	if (err_cnt.rx_err_cnt >= 96U || err_cnt.tx_err_cnt >= 96U) {
		CO_errorReport(em, CO_EM_CAN_BUS_WARNING, CO_EMC_NO_ERROR, errors);
	} else {
		CO_errorReset(em, CO_EM_CAN_BUS_WARNING, errors);
	}

	if (err_cnt.rx_err_cnt >= 128U) {
		CO_errorReport(em, CO_EM_CAN_RX_BUS_PASSIVE, CO_EMC_CAN_PASSIVE, errors);
	} else {
		CO_errorReset(em, CO_EM_CAN_RX_BUS_PASSIVE, errors);
	}

	if (err_cnt.tx_err_cnt >= 128U && !CANmodule->first_tx_msg) {
		CO_errorReport(em, CO_EM_CAN_TX_BUS_PASSIVE, CO_EMC_CAN_PASSIVE, errors);
	} else if (CO_isError(em, CO_EM_CAN_TX_BUS_PASSIVE)) {
		CO_errorReset(em, CO_EM_CAN_TX_BUS_PASSIVE, errors);
		CO_errorReset(em, CO_EM_CAN_TX_OVERFLOW, errors);
	}
}
