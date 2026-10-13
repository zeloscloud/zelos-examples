/*
 * AUTOSAR COM on CAN, the sending side.
 *
 * Signals are packed into I-PDUs from tables generated from the system
 * description (arxml/can-full.arxml, tools/arxml2c.py), then protected or
 * wrapped as the description says: E2E profiles 1, 2 and 5 (AUTOSAR PRS E2E
 * Protocol), a container I-PDU with short headers, a multiplexed I-PDU, or a
 * SecOC secured I-PDU (SWS SecOC).
 *
 * zelos_ar_*, zelos_e2e_* and zelos_secoc_* are the core: pure functions over
 * caller buffers, unit-tested on native_sim. zelos_autosar_start() sends a
 * schedule of PDUs on lib/can; zelos_autosar_cmac() is AES-128 CMAC through
 * PSA Crypto.
 *
 * Little-endian (MOST-SIGNIFICANT-BYTE-LAST) signals and segments only, and
 * byte-aligned SecOC fields: the generator refuses any other description.
 */

#ifndef ZELOS_AUTOSAR_H_
#define ZELOS_AUTOSAR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Signals ------------------------------------------------------------- */

/* An I-PDU's static properties. */
struct zelos_ar_pdu {
	uint16_t length; /* bytes */
	uint8_t unused;  /* UNUSED-BIT-PATTERN, the fill of bits no signal covers */
};

/* One ISignal as mapped into an I-PDU. */
struct zelos_ar_signal {
	uint16_t start; /* START-POSITION, the least significant bit */
	uint8_t length; /* bits, 1..32 */
	float factor;   /* physical = raw * factor + offset */
	float offset;
	/* Raw range of the scale, signed or not, within +-2^24; values beyond it are clamped. */
	int32_t min;
	int32_t max;
	bool has_invalid; /* NaN is sent as invalid (a TEXTTABLE value such as Unknown) */
	uint32_t invalid;
};

/* Fill pdu with its unused bit pattern, before packing signals into it. */
void zelos_ar_init(uint8_t *pdu, const struct zelos_ar_pdu *desc);

/* The raw value of a physical one: scaled, rounded to nearest, clamped. */
uint32_t zelos_ar_raw(const struct zelos_ar_signal *signal, float value);

/* Write raw (its low signal->length bits) at the signal's position. */
void zelos_ar_pack(uint8_t *pdu, const struct zelos_ar_signal *signal, uint32_t raw);

/* zelos_ar_pack() of zelos_ar_raw(). */
void zelos_ar_set(uint8_t *pdu, const struct zelos_ar_signal *signal, float value);

/* --- E2E (PRS E2E Protocol) ---------------------------------------------- */

/* E2E_P01DataIDMode: how the 16-bit Data ID enters the CRC (PRS_E2E_00163). */
enum zelos_e2e_p01_data_id_mode {
	ZELOS_E2E_P01_DATAID_BOTH,   /* all16Bit: low byte, then high byte */
	ZELOS_E2E_P01_DATAID_ALT,    /* alternating8Bit: low on even counters, high on odd */
	ZELOS_E2E_P01_DATAID_LOW,    /* lower8Bit: low byte only */
	ZELOS_E2E_P01_DATAID_NIBBLE, /* lower12Bit: low byte and 0x00; high nibble sent */
};

/* Offsets and lengths in bits, as E2E_P01ConfigType. */
struct zelos_e2e_p01_config {
	uint16_t data_id;
	uint8_t data_id_mode;
	uint16_t counter_offset;        /* multiple of 4 */
	uint16_t crc_offset;            /* multiple of 8 */
	uint16_t data_id_nibble_offset; /* multiple of 4, NIBBLE only */
	uint16_t data_length;
};

/*
 * Profile 1: counter (0..14) at counter_offset, CRC-8 SAE J1850 with start and
 * final XOR 0x00 over the Data ID and every byte but the CRC (PRS_E2E_00070,
 * 00082, 00640), CRC at crc_offset, then the counter advances (PRS_E2E_00075).
 */
void zelos_e2e_p01_protect(const struct zelos_e2e_p01_config *config, uint8_t *counter,
			   uint8_t *data);

/* As E2E_P02ConfigType; the layout is fixed: CRC in byte 0, counter in byte 1 bits 0..3. */
struct zelos_e2e_p02_config {
	uint8_t data_ids[16]; /* DataIDList, indexed by the counter */
	uint16_t data_length; /* bits */
};

/*
 * Profile 2: the counter advances first (0..15, PRS Table 6.36), then
 * CRC-8H2F (0x2F, start and final XOR 0xFF) over bytes 1.. and the counter's
 * Data ID goes into byte 0.
 */
void zelos_e2e_p02_protect(const struct zelos_e2e_p02_config *config, uint8_t *counter,
			   uint8_t *data);

/* As E2E_P05ConfigType; offset and length in bits. */
struct zelos_e2e_p05_config {
	uint16_t data_id;
	uint16_t offset; /* multiple of 8 */
	uint16_t data_length;
};

/*
 * Profile 5: counter (0..255) after the CRC (PRS_E2E_00397), CRC-16
 * CCITT-FALSE (Crc_CalculateCRC16, PRS_E2E_00400) over all but the CRC, then
 * the Data ID low byte and high byte (PRS_E2E_00399), little-endian at offset.
 */
void zelos_e2e_p05_protect(const struct zelos_e2e_p05_config *config, uint8_t *counter,
			   uint8_t *data);

/* --- Container and multiplexed I-PDUs ------------------------------------ */

/*
 * A container I-PDU holding one contained I-PDU behind a short header: the
 * 24-bit header id and the 8-bit length, most significant byte first. out
 * holds 4 + len bytes; returns that length.
 */
size_t zelos_ar_contain(uint8_t *out, uint32_t header_id, const uint8_t *pdu, uint8_t len);

/* Bits start..start + length - 1 of a multiplexed I-PDU (SEGMENT-POSITION). */
struct zelos_ar_segment {
	uint16_t start;
	uint16_t length;
};

/*
 * Copy the segments of a part (the static part, or a dynamic part alternative,
 * whose layout spans the whole multiplexed I-PDU) into the multiplexed I-PDU.
 */
void zelos_ar_merge(uint8_t *mux, const uint8_t *part, const struct zelos_ar_segment *segments,
		    size_t count);

/* --- SecOC (SWS SecOC) ---------------------------------------------------- */

/*
 * DataToAuthenticator (SWS_SecOC_00034): the 16-bit Data Id, the payload and
 * the complete freshness value of freshness_bits (a multiple of 8, at most
 * 32), Data Id and freshness big-endian. Returns its length.
 */
size_t zelos_secoc_auth_data(uint8_t *out, uint16_t data_id, const uint8_t *payload,
			     size_t payload_len, uint32_t freshness, uint8_t freshness_bits);

/*
 * Complete a secured I-PDU whose first payload_len bytes hold the payload:
 * the freshness value's freshness_tx_bits least significant bits, then the
 * MAC's mac_tx_bits most significant bits (SWS_SecOC_00036), both multiples
 * of 8.
 */
void zelos_secoc_trailer(uint8_t *pdu, size_t payload_len, uint32_t freshness,
			 uint8_t freshness_tx_bits, const uint8_t *mac, uint8_t mac_tx_bits);

/* --- Zephyr --------------------------------------------------------------- */

/* One I-PDU on its own timing. */
struct zelos_ar_tx {
	uint32_t id;        /* standard CAN identifier of the frame it leaves in */
	uint16_t period_ms; /* CYCLIC-TIMING TIME-PERIOD */
	uint16_t offset_ms; /* TIME-OFFSET, from zelos_autosar_start() */
	/* Build the frame into data (8 bytes); return its length, 0 to send none this cycle. */
	uint8_t (*build)(uint8_t *data);
};

/*
 * Send each entry every period_ms from offset_ms on, on one thread, through
 * lib/can. A cycle the thread misses is skipped, not sent late in a burst.
 * schedule must outlive the node; at most CONFIG_ZELOS_AUTOSAR_TX_MAX entries.
 *
 * @return 0, or negative errno.
 */
int zelos_autosar_start(const struct zelos_ar_tx *schedule, size_t count);

/*
 * AES-128 CMAC (NIST SP 800-38B) of data under key, through PSA Crypto.
 *
 * @return 0, or negative errno.
 */
int zelos_autosar_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16]);

#endif /* ZELOS_AUTOSAR_H_ */
