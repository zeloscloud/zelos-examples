/* AES-128 CMAC for SecOC, through PSA Crypto (Mbed TLS / TF-PSA-Crypto). */

#include <zelos/autosar.h>

#include <errno.h>

#include <psa/crypto.h>

int zelos_autosar_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	size_t mac_len;
	psa_status_t status;

	/* Idempotent: initialises once, then returns success. */
	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
	if (psa_import_key(&attr, key, 16, &id) != PSA_SUCCESS) {
		return -EIO;
	}
	/* A volatile key per call: one secured PDU at a few Hz, and no key state to keep. */
	status = psa_mac_compute(id, PSA_ALG_CMAC, data, len, mac, 16, &mac_len);
	(void)psa_destroy_key(id);

	return status == PSA_SUCCESS && mac_len == 16 ? 0 : -EIO;
}
