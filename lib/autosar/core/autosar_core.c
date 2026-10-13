/*
 * The AUTOSAR COM core: signal packing, E2E profiles 1, 2 and 5, container
 * and multiplexed I-PDUs, the SecOC trailer. No I/O; see include/zelos/autosar.h.
 */

#include <zelos/autosar.h>

#include <math.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* --- Signals ------------------------------------------------------------- */

void zelos_ar_init(uint8_t *pdu, const struct zelos_ar_pdu *desc)
{
	memset(pdu, desc->unused, desc->length);
}

uint32_t zelos_ar_raw(const struct zelos_ar_signal *signal, float value)
{
	const uint32_t mask = signal->length == 32 ? UINT32_MAX : BIT(signal->length) - 1U;
	float raw;

	if (isnan(value)) {
		return signal->has_invalid ? signal->invalid : (uint32_t)signal->min & mask;
	}
	raw = roundf((value - signal->offset) / signal->factor);
	/* Clamped as a float, so a huge value cannot overflow the conversion. */
	raw = CLAMP(raw, (float)signal->min, (float)signal->max);
	return (uint32_t)(int32_t)raw & mask;
}

void zelos_ar_pack(uint8_t *pdu, const struct zelos_ar_signal *signal, uint32_t raw)
{
	for (uint8_t i = 0; i < signal->length; i++) {
		const unsigned int bit = signal->start + i;

		WRITE_BIT(pdu[bit / 8], bit % 8, (raw >> i) & 1U);
	}
}

void zelos_ar_set(uint8_t *pdu, const struct zelos_ar_signal *signal, float value)
{
	zelos_ar_pack(pdu, signal, zelos_ar_raw(signal, value));
}

/* --- E2E ------------------------------------------------------------------ */

#define P01_POLY 0x1D
#define P02_POLY 0x2F

/* Write a nibble at a bit offset that is a multiple of 4 (PRS Figure 6.55). */
static void write_nibble(uint8_t *data, uint16_t offset, uint8_t value)
{
	uint8_t *byte = &data[offset / 8];

	if (offset % 8 == 0) {
		*byte = (*byte & 0xF0) | (value & 0x0F);
	} else {
		*byte = (*byte & 0x0F) | ((value << 4) & 0xF0);
	}
}

void zelos_e2e_p01_protect(const struct zelos_e2e_p01_config *config, uint8_t *counter,
			   uint8_t *data)
{
	const uint8_t id[2] = {config->data_id & 0xFF, config->data_id >> 8};
	const size_t crc_byte = config->crc_offset / 8;
	const size_t len = config->data_length / 8;
	uint8_t crc = 0x00;

	write_nibble(data, config->counter_offset, *counter);
	if (config->data_id_mode == ZELOS_E2E_P01_DATAID_NIBBLE) {
		write_nibble(data, config->data_id_nibble_offset, config->data_id >> 8);
	}

	/* Start value 0x00, no final XOR: PRS_E2E_00070. */
	switch (config->data_id_mode) {
	case ZELOS_E2E_P01_DATAID_BOTH:
		crc = crc8(id, 2, P01_POLY, crc, false);
		break;
	case ZELOS_E2E_P01_DATAID_ALT:
		crc = crc8(&id[*counter % 2], 1, P01_POLY, crc, false);
		break;
	case ZELOS_E2E_P01_DATAID_LOW:
		crc = crc8(&id[0], 1, P01_POLY, crc, false);
		break;
	case ZELOS_E2E_P01_DATAID_NIBBLE:
		/* The low byte, then a zero byte: PRS_E2E_00640. */
		crc = crc8(&id[0], 1, P01_POLY, crc, false);
		crc = crc8(&(uint8_t){0x00}, 1, P01_POLY, crc, false);
		break;
	}
	crc = crc8(data, crc_byte, P01_POLY, crc, false);
	crc = crc8(&data[crc_byte + 1], len - crc_byte - 1, P01_POLY, crc, false);
	data[crc_byte] = crc;

	/* 0..14: 0xF is skipped (PRS_E2E_00075). */
	*counter = (*counter + 1) % 15;
}

void zelos_e2e_p02_protect(const struct zelos_e2e_p02_config *config, uint8_t *counter,
			   uint8_t *data)
{
	uint8_t crc;

	/* Incremented before use: the first frame carries 1 (PRS Table 6.36). */
	*counter = (*counter + 1) % 16;
	data[1] = (data[1] & 0xF0) | *counter;

	crc = crc8(&data[1], config->data_length / 8 - 1, P02_POLY, 0xFF, false);
	crc = crc8(&config->data_ids[*counter], 1, P02_POLY, crc, false);
	data[0] = crc ^ 0xFF;
}

void zelos_e2e_p05_protect(const struct zelos_e2e_p05_config *config, uint8_t *counter,
			   uint8_t *data)
{
	const size_t at = config->offset / 8;
	const size_t len = config->data_length / 8;
	const uint8_t id[2] = {config->data_id & 0xFF, config->data_id >> 8};
	uint16_t crc;

	data[at + 2] = *counter;

	/* Crc_CalculateCRC16: poly 0x1021, start 0xFFFF, no final XOR. */
	crc = crc16_itu_t(0xFFFF, data, at);
	crc = crc16_itu_t(crc, &data[at + 2], len - at - 2);
	crc = crc16_itu_t(crc, id, 2);
	data[at] = crc & 0xFF;
	data[at + 1] = crc >> 8;

	*counter = *counter + 1; /* wraps 255 to 0 */
}

/* --- Container and multiplexed I-PDUs ------------------------------------ */

size_t zelos_ar_contain(uint8_t *out, uint32_t header_id, const uint8_t *pdu, uint8_t len)
{
	out[0] = header_id >> 16;
	out[1] = header_id >> 8;
	out[2] = header_id;
	out[3] = len;
	memcpy(&out[4], pdu, len);
	return 4U + len;
}

void zelos_ar_merge(uint8_t *mux, const uint8_t *part, const struct zelos_ar_segment *segments,
		    size_t count)
{
	for (size_t s = 0; s < count; s++) {
		for (uint16_t i = 0; i < segments[s].length; i++) {
			const unsigned int bit = segments[s].start + i;

			WRITE_BIT(mux[bit / 8], bit % 8, (part[bit / 8] >> (bit % 8)) & 1U);
		}
	}
}

/* --- SecOC ---------------------------------------------------------------- */

/* The n / 8 low bytes of value, most significant first. */
static void put_be(uint8_t *out, uint32_t value, uint8_t bits)
{
	for (uint8_t i = 0; i < bits / 8; i++) {
		out[i] = value >> (bits - 8 * (i + 1));
	}
}

size_t zelos_secoc_auth_data(uint8_t *out, uint16_t data_id, const uint8_t *payload,
			     size_t payload_len, uint32_t freshness, uint8_t freshness_bits)
{
	put_be(out, data_id, 16);
	memcpy(&out[2], payload, payload_len);
	put_be(&out[2 + payload_len], freshness, freshness_bits);
	return 2 + payload_len + freshness_bits / 8;
}

void zelos_secoc_trailer(uint8_t *pdu, size_t payload_len, uint32_t freshness,
			 uint8_t freshness_tx_bits, const uint8_t *mac, uint8_t mac_tx_bits)
{
	put_be(&pdu[payload_len], freshness, freshness_tx_bits);
	memcpy(&pdu[payload_len + freshness_tx_bits / 8], mac, mac_tx_bits / 8);
}
