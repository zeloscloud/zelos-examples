/*
 * The AUTOSAR core against the specifications' own examples: PRS E2E Protocol
 * (R22-11) Tables 6.20-6.27, 6.36, 6.53 and 6.54; RFC 4493 for the CMAC. Then
 * signal scaling at its edges. The container, multiplexed and SecOC layouts are
 * the autosar suite's: it decodes the node's frames from the ARXML.
 */

#include <zelos/autosar.h>

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#define ZEROS {0, 0, 0, 0, 0, 0, 0, 0}

/* --- E2E ------------------------------------------------------------------ */

/* Table 6.18: CounterOffset 8, CRCOffset 0, DataID 0x123, DataIDNibbleOffset 12, DataLength 64. */
static void p01(uint8_t mode, const uint8_t expect[2][2])
{
	const struct zelos_e2e_p01_config config = {
		.data_id = 0x123,
		.data_id_mode = mode,
		.counter_offset = 8,
		.crc_offset = 0,
		.data_id_nibble_offset = 12,
		.data_length = 64,
	};
	uint8_t counter = 0;

	for (int n = 0; n < 2; n++) {
		uint8_t data[8] = ZEROS;

		zelos_e2e_p01_protect(&config, &counter, data);
		zassert_equal(data[0], expect[n][0], "mode %u counter %d: CRC 0x%02x", mode, n,
			      data[0]);
		zassert_equal(data[1], expect[n][1], "mode %u counter %d: byte 1 0x%02x", mode, n,
			      data[1]);
	}
}

ZTEST(autosar, test_e2e_p01_spec_examples)
{
	/* Tables 6.20/6.21, 6.22/6.23, 6.24/6.25, 6.26/6.27: data all zeros, counters 0 and 1. */
	p01(ZELOS_E2E_P01_DATAID_BOTH, (const uint8_t[2][2]){{0xCC, 0x00}, {0x91, 0x01}});
	p01(ZELOS_E2E_P01_DATAID_ALT, (const uint8_t[2][2]){{0xCE, 0x00}, {0x02, 0x01}});
	p01(ZELOS_E2E_P01_DATAID_LOW, (const uint8_t[2][2]){{0xCE, 0x00}, {0x93, 0x01}});
	p01(ZELOS_E2E_P01_DATAID_NIBBLE, (const uint8_t[2][2]){{0x2A, 0x10}, {0x77, 0x11}});
}

ZTEST(autosar, test_e2e_p01_counter_skips_15)
{
	const struct zelos_e2e_p01_config config = {
		.data_id = 0x123, .counter_offset = 8, .data_length = 64};
	uint8_t data[8] = ZEROS;
	uint8_t counter = 14;

	zelos_e2e_p01_protect(&config, &counter, data);
	zassert_equal(data[1] & 0x0F, 14);
	zassert_equal(counter, 0, "PRS_E2E_00075: 0xF is skipped");
}

ZTEST(autosar, test_e2e_p02_spec_example)
{
	/* Table 6.34: DataIDList 0x01..0x10; Table 6.36: CRC for counters 1..15, then 0. */
	static const uint8_t crc[16] = {0x0E, 0x1B, 0x98, 0x31, 0x0D, 0x18, 0x9B, 0x65,
					0x08, 0x1D, 0x9E, 0x37, 0x0B, 0x1E, 0x9D, 0xCD};
	struct zelos_e2e_p02_config config = {.data_length = 64};
	uint8_t counter = 0;

	for (int i = 0; i < 16; i++) {
		config.data_ids[i] = i + 1;
	}
	for (int n = 1; n <= 16; n++) {
		uint8_t data[8] = ZEROS;

		zelos_e2e_p02_protect(&config, &counter, data);
		zassert_equal(data[1], n % 16, "counter incremented before use");
		zassert_equal(data[0], crc[n % 16], "counter %d: CRC 0x%02x", n % 16, data[0]);
	}
}

ZTEST(autosar, test_e2e_p05_spec_examples)
{
	/* Table 6.51: DataID 0x1234; counter 0. */
	struct zelos_e2e_p05_config config = {.data_id = 0x1234, .offset = 0, .data_length = 64};
	uint8_t data[16] = {0};
	uint8_t counter = 0;

	/* Table 6.53: 8 bytes, offset 0. */
	zelos_e2e_p05_protect(&config, &counter, data);
	zassert_mem_equal(data, ((uint8_t[]){0x1C, 0xCA, 0x00}), 3);
	zassert_equal(counter, 1);

	/* Table 6.54: 16 bytes, offset 64 (behind a SOME/IP header). */
	memset(data, 0, sizeof(data));
	config.offset = 64;
	config.data_length = 128;
	counter = 0;
	zelos_e2e_p05_protect(&config, &counter, data);
	zassert_mem_equal(&data[8], ((uint8_t[]){0x28, 0x91, 0x00}), 3);
	zassert_mem_equal(data, ((uint8_t[8])ZEROS), 8, "header before the offset untouched");

	counter = 255;
	zelos_e2e_p05_protect(&config, &counter, data);
	zassert_equal(data[10], 255);
	zassert_equal(counter, 0, "PRS_E2E_00397: 0xFF is a normal value, then 0");
}

/* --- Signals ------------------------------------------------------------- */

ZTEST(autosar, test_signal_scaling)
{
	static const struct zelos_ar_signal temperature = {
		.start = 16,
		.length = 16,
		.factor = 0.1f,
		.min = -400,
		.max = 2150,
	};
	static const struct zelos_ar_signal voltage = {
		.start = 40,
		.length = 16,
		.factor = 0.1f,
		.max = 65534,
		.has_invalid = true,
		.invalid = 0xFFFF,
	};
	static const struct zelos_ar_signal state = {
		.start = 12, .length = 4, .factor = 1, .max = 15};
	uint8_t pdu[8];

	zelos_ar_init(pdu, &(struct zelos_ar_pdu){.length = 8, .unused = 0xFF});
	zelos_ar_set(pdu, &temperature, -12.34f); /* -123: rounded, two's complement */
	zelos_ar_set(pdu, &voltage, NAN);         /* Unknown */
	zelos_ar_pack(pdu, &state, 2);
	zassert_mem_equal(pdu, ((uint8_t[]){0xFF, 0x2F, 0x85, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), 8);

	zassert_equal(zelos_ar_raw(&temperature, 1000.0f), 2150, "clamped high");
	zassert_equal(zelos_ar_raw(&temperature, -1000.0f), (uint16_t)-400, "clamped low");
	zassert_equal(zelos_ar_raw(&voltage, 374.06f), 3741, "rounded to nearest");
}

/* --- SecOC ---------------------------------------------------------------- */

/* RFC 4493's key: the SecOC demo key of the DC-DC too. */
static const uint8_t key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
				0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};

ZTEST(autosar, test_cmac_rfc4493)
{
	static const uint8_t m16[16] = {0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96,
					0xE9, 0x3D, 0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A};
	uint8_t mac[16];

	/* RFC 4493 4. Examples 1 and 2. */
	zassert_ok(zelos_autosar_cmac(key, NULL, 0, mac));
	zassert_mem_equal(mac,
			  ((uint8_t[]){0xBB, 0x1D, 0x69, 0x29, 0xE9, 0x59, 0x37, 0x28, 0x7F, 0xA3,
				       0x7D, 0x12, 0x9B, 0x75, 0x67, 0x46}),
			  16);
	zassert_ok(zelos_autosar_cmac(key, m16, sizeof(m16), mac));
	zassert_mem_equal(mac,
			  ((uint8_t[]){0x07, 0x0A, 0x16, 0xB4, 0x6B, 0x4D, 0x41, 0x44, 0xF7, 0x9B,
				       0xDD, 0x9D, 0xD0, 0x4A, 0x28, 0x7C}),
			  16);
}

ZTEST_SUITE(autosar, NULL, NULL, NULL, NULL, NULL);
