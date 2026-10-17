/* Shared by the UDS core and its OBD-II services; nothing else includes this. */

#ifndef ZELOS_UDS_CORE_H_
#define ZELOS_UDS_CORE_H_

#include <zelos/uds.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

enum {
	NRC_INCORRECT_LENGTH = 0x13,
	NRC_RESPONSE_TOO_LONG = 0x14,
	NRC_OUT_OF_RANGE = 0x31,
};

/* ISO 14229-1 Annex D status bits; all supported. */
#define TF BIT(0)
#define TFTOC BIT(1)
#define PDTC BIT(2)
#define CDTC BIT(3)
#define TNCSLC BIT(4)
#define TFSLC BIT(5)
#define TNCTOC BIT(6)
#define WIR BIT(7)
#define DTC_AVAILABILITY 0xFF
#define SNAPSHOT_RECORD 0x01
/* OBDonUDS snapshot records: first frame, second (latest) frame. */
#define UDS_RECORD_FIRST 0x00
#define UDS_RECORD_LATEST 0xF0

/* A response under construction; never beyond CONFIG_ZELOS_UDS_BUF_SIZE. */
struct rsp {
	uint8_t *buf;
	size_t len;
	bool suppress;
};

static inline bool put(struct rsp *r, const void *data, size_t n)
{
	if (n > CONFIG_ZELOS_UDS_BUF_SIZE - r->len) {
		return false;
	}
	memcpy(r->buf + r->len, data, n);
	r->len += n;
	return true;
}

static inline bool put_u8(struct rsp *r, uint8_t v)
{
	return put(r, &v, 1);
}

static inline bool put_be16(struct rsp *r, uint16_t v)
{
	uint8_t b[2];

	sys_put_be16(v, b);
	return put(r, b, sizeof(b));
}

static inline bool put_be24(struct rsp *r, uint32_t v)
{
	uint8_t b[3];

	sys_put_be24(v, b);
	return put(r, b, sizeof(b));
}

static inline bool put_be32(struct rsp *r, uint32_t v)
{
	uint8_t b[4];

	sys_put_be32(v, b);
	return put(r, b, sizeof(b));
}

const struct zelos_uds_did *uds_find_did(const struct zelos_uds_config *cfg, uint16_t id);

/* DTC i as ISO 14229-1 reports it: 3 bytes and its status. */
bool uds_put_dtc(struct zelos_uds_core *uds, size_t i, struct rsp *r);

/* Clear a DTC (ClearDTC, OBD 0x04): erased, never tested since; permanent stays. */
void uds_clear_record(struct zelos_uds_dtc_record *d);

/* Offset of a DID's value in snapshot record 0x01, or -1 if not captured. */
int uds_snapshot_offset(const struct zelos_uds_config *cfg, uint16_t did);

#if defined(CONFIG_ZELOS_UDS_OBD)
/* J1979 services 0x01-0x0A. @return response length; 0: nothing. */
size_t uds_obd_request(struct zelos_uds_core *uds, const uint8_t *req, size_t len, uint8_t *rsp);
/* ReadDataByIdentifier of a J1979-2 DID (0xF4xx, 0xF5xx, 0xF8xx). */
bool uds_obd_read_did(struct zelos_uds_core *uds, uint16_t id, struct rsp *r, bool *found);
/* ReadDTCInformation 0x42, 0x55, 0x56. @return NRC, 0 on success. */
uint8_t uds_obd_read_dtc(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			 struct rsp *r);
/* An OBDonUDS request: never 0x13, never 0x21 (CCR 1968.2 (g)(3.4.2)). */
bool uds_obd_relevant(const uint8_t *req, size_t len);
/* Snapshot record 0x00 or 0xF0 from a snapshot: DIDs 0xF400 + PID. */
void uds_obd_put_frame(struct zelos_uds_core *uds, const uint8_t *snapshot, uint8_t record,
		       struct rsp *r);
#endif

#endif /* ZELOS_UDS_CORE_H_ */
