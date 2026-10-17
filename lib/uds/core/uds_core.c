/*
 * UDS server core. No I/O, no Zephyr calls beyond pure helpers: the glue feeds
 * it requests and the clock and sends what it returns.
 */

#include "core.h"

#include <errno.h>

#include <zephyr/sys/crc.h>

enum {
	SID_SESSION_CONTROL = 0x10,
	SID_ECU_RESET = 0x11,
	SID_CLEAR_DTC = 0x14,
	SID_READ_DTC = 0x19,
	SID_READ_DID = 0x22,
	SID_READ_MEMORY = 0x23,
	SID_SECURITY_ACCESS = 0x27,
	SID_COMMUNICATION_CONTROL = 0x28,
	SID_WRITE_DID = 0x2E,
	SID_IO_CONTROL = 0x2F,
	SID_ROUTINE_CONTROL = 0x31,
	SID_REQUEST_DOWNLOAD = 0x34,
	SID_REQUEST_UPLOAD = 0x35,
	SID_TRANSFER_DATA = 0x36,
	SID_TRANSFER_EXIT = 0x37,
	SID_WRITE_MEMORY = 0x3D,
	SID_TESTER_PRESENT = 0x3E,
	SID_DTC_SETTING = 0x85,
};

#define NEGATIVE 0x7F
#define POSITIVE 0x40
#define SUPPRESS 0x80

enum {
	NRC_SERVICE_NOT_SUPPORTED = 0x11,
	NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12,
	NRC_BUSY_REPEAT = 0x21,
	NRC_CONDITIONS = 0x22,
	NRC_SEQUENCE = 0x24,
	NRC_SECURITY_DENIED = 0x33,
	NRC_INVALID_KEY = 0x35,
	NRC_ATTEMPTS_EXCEEDED = 0x36,
	NRC_DELAY_NOT_EXPIRED = 0x37,
	NRC_DOWNLOAD_NOT_ACCEPTED = 0x70,
	NRC_TRANSFER_SUSPENDED = 0x71,
	NRC_WRONG_BSC = 0x73,
	NRC_PENDING = 0x78,
	NRC_SUBFUNCTION_NOT_IN_SESSION = 0x7E,
	NRC_SERVICE_NOT_IN_SESSION = 0x7F,
};

/* P2 50 ms, P2* 5000 ms (in 10 ms), as DiagnosticSessionControl reports them. */
#define P2_MS 50
#define P2_STAR_MS 5000
#define S3_MS 5000
/* 0x78 repeats well inside P2*, so a tester never times out on a long routine. */
#define PENDING_REPEAT_MS 2000
#define ERASE_MS 1000

#define SECURITY_ATTEMPTS 3
#define SECURITY_DELAY_MS 10000

#define DTC_CLEARED (TNCSLC | TNCTOC)
#define DTC_FORMAT_ISO14229 0x01
#define DTC_GROUP_ALL 0xFFFFFF
/* ISO 14229-1 group of emissions-related DTCs: every DTC here. */
#define DTC_GROUP_EMISSIONS 0xFFFF33
#define CONFIRM_CYCLES CONFIG_ZELOS_UDS_DTC_CONFIRM_CYCLES
#define HEALING_CYCLES CONFIG_ZELOS_UDS_DTC_HEALING_CYCLES
#define AGING_CYCLES CONFIG_ZELOS_UDS_DTC_AGING_CYCLES
#define RECORD_OCCURRENCES 0x01
#define RECORD_AGING 0x02
#define RECORD_ALL 0xFF

#define MEMORY_MAGIC 0x55445332U /* "UDS2" */

BUILD_ASSERT(ZELOS_UDS_DTCS_MAX <= 32, "permanent DTCs are a 32-bit mask");

#define ALFID_DEFAULT 0x44
#define LENGTH_FORMAT 0x20
#define DOWNLOAD_ADDRESS CONFIG_ZELOS_UDS_DOWNLOAD_ADDRESS

#define S_DEFAULT BIT(ZELOS_UDS_SESSION_DEFAULT)
#define S_PROGRAMMING BIT(ZELOS_UDS_SESSION_PROGRAMMING)
#define S_EXTENDED BIT(ZELOS_UDS_SESSION_EXTENDED)
#define S_NON_DEFAULT (S_PROGRAMMING | S_EXTENDED)
#define S_ANY (S_DEFAULT | S_NON_DEFAULT)

static uint32_t get_be(const uint8_t *p, uint8_t n)
{
	uint32_t v = 0;

	while (n-- > 0) {
		v = v << 8 | *p++;
	}
	return v;
}

/* [addr, addr + size) inside [base, base + limit). Overflow-safe. */
static bool within(uint32_t base, uint32_t limit, uint32_t addr, uint32_t size)
{
	return addr >= base && size <= limit && addr - base <= limit - size;
}

/* Index of a security level's attempt counter and delay. */
static int level_index(uint8_t level)
{
	return level == ZELOS_UDS_LEVEL_PROGRAMMING ? 1 : 0;
}

/* The session each level is requested in. */
static uint8_t level_session(uint8_t level)
{
	return level == ZELOS_UDS_LEVEL_PROGRAMMING ? ZELOS_UDS_SESSION_PROGRAMMING
						    : ZELOS_UDS_SESSION_EXTENDED;
}

const struct zelos_uds_did *uds_find_did(const struct zelos_uds_config *cfg, uint16_t id)
{
	for (size_t i = 0; i < cfg->did_count; i++) {
		if (cfg->dids[i].id == id) {
			return &cfg->dids[i];
		}
	}
	return NULL;
}

static int find_dtc(const struct zelos_uds_config *cfg, uint32_t code)
{
	for (size_t i = 0; i < cfg->dtc_count; i++) {
		if (cfg->dtcs[i] == code) {
			return (int)i;
		}
	}
	return -1;
}

static void release_ios(struct zelos_uds_core *uds)
{
	const struct zelos_uds_config *cfg = uds->config;

	for (size_t i = 0; i < cfg->io_count; i++) {
		if (uds->io_active & BIT(i)) {
			(void)cfg->ios[i].control(ZELOS_UDS_IO_RETURN, NULL);
		}
	}
	uds->io_active = 0;
}

/*
 * Every DiagnosticSessionControl, and S3: lock security, return the outputs,
 * end a transfer. Default also restores communication and DTC setting.
 * Lockout survives, so toggling sessions cannot buy more key attempts.
 */
static void enter_session(struct zelos_uds_core *uds, uint8_t session)
{
	uds->session = session;
	uds->unlocked = 0;
	uds->seed = 0;
	release_ios(uds);
	uds->transfer = ZELOS_UDS_TRANSFER_NONE;
	if (session == ZELOS_UDS_SESSION_DEFAULT) {
		uds->communication = 0x00;
		uds->dtc_setting = true;
	}
}

static uint8_t session_control(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			       struct rsp *r, int64_t now)
{
	uint8_t session = req[1] & ~SUPPRESS;

	ARG_UNUSED(now);

	if (session < ZELOS_UDS_SESSION_DEFAULT || session > ZELOS_UDS_SESSION_EXTENDED) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != 2) {
		return NRC_INCORRECT_LENGTH;
	}
	if (session == ZELOS_UDS_SESSION_PROGRAMMING &&
	    uds->session == ZELOS_UDS_SESSION_DEFAULT) {
		return NRC_CONDITIONS;
	}

	enter_session(uds, session);
	put_u8(r, session);
	put_be16(r, P2_MS);
	put_be16(r, P2_STAR_MS / 10);
	return 0;
}

static uint8_t ecu_reset(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			 struct rsp *r, int64_t now)
{
	uint8_t type = req[1] & ~SUPPRESS;

	ARG_UNUSED(now);

	/* Hard, key off/on, soft. */
	if (type < 0x01 || type > 0x03) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != 2) {
		return NRC_INCORRECT_LENGTH;
	}

	uds->reset = type == 0x03 ? ZELOS_UDS_RESET_SOFT : ZELOS_UDS_RESET_HARD;
	put_u8(r, type);
	return 0;
}

static uint8_t tester_present(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			      struct rsp *r, int64_t now)
{
	ARG_UNUSED(uds);
	ARG_UNUSED(now);

	if ((req[1] & ~SUPPRESS) != 0x00) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != 2) {
		return NRC_INCORRECT_LENGTH;
	}

	put_u8(r, 0x00);
	return 0;
}

static uint8_t security_access(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			       struct rsp *r, int64_t now)
{
	uint8_t type = req[1] & ~SUPPRESS;
	/* requestSeed is odd, sendKey the next even. */
	uint8_t level = (type & 1) != 0 ? type : type - 1;
	int i = level_index(level);
	uint32_t xor;

	if (level != ZELOS_UDS_LEVEL_EXTENDED && level != ZELOS_UDS_LEVEL_PROGRAMMING) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (uds->session != level_session(level)) {
		return NRC_SUBFUNCTION_NOT_IN_SESSION;
	}

	if (type == level) {
		if (len != 2) {
			return NRC_INCORRECT_LENGTH;
		}
		if (now < uds->lockout_until[i]) {
			return NRC_DELAY_NOT_EXPIRED;
		}
		/* The delay over, a fresh set of attempts. */
		if (uds->lockout_until[i] != 0) {
			uds->lockout_until[i] = 0;
			uds->failed_keys[i] = 0;
		}
		/* Already unlocked: a zero seed says so. */
		if (uds->unlocked == level) {
			uds->seed = 0;
		} else {
			do {
				uds->seed = uds->random();
			} while (uds->seed == 0);
			uds->seed_level = level;
		}
		put_u8(r, type);
		put_be32(r, uds->seed);
		return 0;
	}

	if (len != 6) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->seed == 0 || uds->seed_level != level) {
		return NRC_SEQUENCE;
	}

	xor = level == ZELOS_UDS_LEVEL_PROGRAMMING ? ZELOS_UDS_KEY_XOR_PROGRAMMING
						   : ZELOS_UDS_KEY_XOR_EXTENDED;
	if (sys_get_be32(&req[2]) != (uds->seed ^ xor)) {
		/* A wrong key spends the seed. */
		uds->seed = 0;
		if (++uds->failed_keys[i] >= SECURITY_ATTEMPTS) {
			uds->lockout_until[i] = now + SECURITY_DELAY_MS;
			return NRC_ATTEMPTS_EXCEEDED;
		}
		return NRC_INVALID_KEY;
	}

	uds->seed = 0;
	uds->failed_keys[i] = 0;
	uds->unlocked = level;
	put_u8(r, type);
	return 0;
}

static uint8_t communication_control(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
				     struct rsp *r, int64_t now)
{
	uint8_t type = req[1] & ~SUPPRESS;

	ARG_UNUSED(now);

	/* enableRxAndTx, enableRxAndDisableTx, disableRxAndTx. */
	if (type != 0x00 && type != 0x01 && type != 0x03) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != 3) {
		return NRC_INCORRECT_LENGTH;
	}
	/* Normal communication messages only. */
	if (req[2] != 0x01) {
		return NRC_OUT_OF_RANGE;
	}
	if (uds->unlocked == 0) {
		return NRC_SECURITY_DENIED;
	}

	uds->communication = type;
	put_u8(r, type);
	return 0;
}

static uint8_t dtc_setting(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			   struct rsp *r, int64_t now)
{
	uint8_t type = req[1] & ~SUPPRESS;

	ARG_UNUSED(now);

	if (type != 0x01 && type != 0x02) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != 2) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->unlocked == 0) {
		return NRC_SECURITY_DENIED;
	}

	uds->dtc_setting = type == 0x01;
	put_u8(r, type);
	return 0;
}

static bool read_one(struct zelos_uds_core *uds, uint16_t id, struct rsp *r, bool *found)
{
	const struct zelos_uds_did *did = uds_find_did(uds->config, id);
	uint8_t value[UINT8_MAX];

	*found = true;
	if (id == ZELOS_UDS_DID_ACTIVE_SESSION) {
		return put_be16(r, id) && put_u8(r, uds->session);
	}
	if (did == NULL) {
#if defined(CONFIG_ZELOS_UDS_OBD)
		return uds_obd_read_did(uds, id, r, found);
#else
		*found = false;
		return true;
#endif
	}
	did->read(value);
	return put_be16(r, id) && put(r, value, did->size);
}

static uint8_t read_did(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			struct rsp *r, int64_t now)
{
	bool found, any = false;

	ARG_UNUSED(now);

	if (len < 3 || (len - 1) % 2 != 0) {
		return NRC_INCORRECT_LENGTH;
	}
	/* ISO 14229-1: the DIDs supported, 0x31 only if none is. */
	for (size_t i = 1; i < len; i += 2) {
		if (!read_one(uds, sys_get_be16(&req[i]), r, &found)) {
			return NRC_RESPONSE_TOO_LONG;
		}
		any |= found;
	}
	return any ? 0 : NRC_OUT_OF_RANGE;
}

static uint8_t write_did(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			 struct rsp *r, int64_t now)
{
	const struct zelos_uds_did *did;
	uint16_t id;
	uint8_t nrc;

	ARG_UNUSED(now);

	if (len < 4) {
		return NRC_INCORRECT_LENGTH;
	}
	id = sys_get_be16(&req[1]);
	did = uds_find_did(uds->config, id);
	if (did == NULL || did->write == NULL || uds->session != ZELOS_UDS_SESSION_EXTENDED) {
		return NRC_OUT_OF_RANGE;
	}
	if (uds->unlocked != ZELOS_UDS_LEVEL_EXTENDED) {
		return NRC_SECURITY_DENIED;
	}
	if (len != 3U + did->size) {
		return NRC_INCORRECT_LENGTH;
	}
	nrc = did->write(&req[3]);
	if (nrc != 0) {
		return nrc;
	}
	put_be16(r, id);
	return 0;
}

/* addressAndLengthFormatIdentifier of a memory request: 4-byte address, 1-, 2- or 4-byte size. */
static bool memory_alfid(uint8_t alfid)
{
	return alfid == 0x44 || alfid == 0x24 || alfid == 0x14;
}

static const struct zelos_uds_memory *find_memory(const struct zelos_uds_config *cfg,
						   uint32_t addr, uint32_t size)
{
	for (size_t i = 0; i < cfg->memory_count; i++) {
		const struct zelos_uds_memory *m = &cfg->memories[i];

		if (size > 0 && within(m->address, m->size, addr, size)) {
			return m;
		}
	}
	return NULL;
}

static uint8_t read_memory(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			   struct rsp *r, int64_t now)
{
	const struct zelos_uds_memory *m;
	uint8_t size_len;
	uint32_t addr, size;

	ARG_UNUSED(now);

	if (len < 4) {
		return NRC_INCORRECT_LENGTH;
	}
	if (!memory_alfid(req[1])) {
		return NRC_OUT_OF_RANGE;
	}
	size_len = req[1] >> 4;
	if (len != 6U + size_len) {
		return NRC_INCORRECT_LENGTH;
	}
	addr = sys_get_be32(&req[2]);
	size = get_be(&req[6], size_len);
	m = find_memory(uds->config, addr, size);
	if (m == NULL) {
		return NRC_OUT_OF_RANGE;
	}
	if (!put(r, (const uint8_t *)m->data + (addr - m->address), size)) {
		return NRC_RESPONSE_TOO_LONG;
	}
	return 0;
}

static uint8_t write_memory(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			    struct rsp *r, int64_t now)
{
	const struct zelos_uds_memory *m;
	uint8_t size_len;
	uint32_t addr, size;

	ARG_UNUSED(now);

	if (len < 5) {
		return NRC_INCORRECT_LENGTH;
	}
	if (!memory_alfid(req[1])) {
		return NRC_OUT_OF_RANGE;
	}
	size_len = req[1] >> 4;
	if (len < 6U + size_len) {
		return NRC_INCORRECT_LENGTH;
	}
	addr = sys_get_be32(&req[2]);
	size = get_be(&req[6], size_len);
	if (len != 6U + size_len + size) {
		return NRC_INCORRECT_LENGTH;
	}
	m = find_memory(uds->config, addr, size);
	if (m == NULL || !m->writable) {
		return NRC_OUT_OF_RANGE;
	}
	if (uds->unlocked != ZELOS_UDS_LEVEL_EXTENDED) {
		return NRC_SECURITY_DENIED;
	}
	if (uds->config->write != NULL) {
		uds->config->write((uint8_t *)m->data + (addr - m->address), &req[6 + size_len],
				   size);
	} else {
		memcpy((uint8_t *)m->data + (addr - m->address), &req[6 + size_len], size);
	}
	put(r, &req[1], 5U + size_len);
	return 0;
}

/* Power-on: nothing stored. */
static void erase_record(struct zelos_uds_dtc_record *d)
{
	memset(d, 0, sizeof(*d));
	d->status = DTC_CLEARED;
}

void uds_clear_record(struct zelos_uds_dtc_record *d)
{
	bool permanent = d->permanent;

	erase_record(d);
	d->permanent = permanent;
}

static uint8_t clear_dtc(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			 struct rsp *r, int64_t now)
{
	const struct zelos_uds_config *cfg = uds->config;
	uint32_t group;
	int i;

	ARG_UNUSED(r);
	ARG_UNUSED(now);

	if (len != 4) {
		return NRC_INCORRECT_LENGTH;
	}
	group = sys_get_be24(&req[1]);
	if (group == DTC_GROUP_ALL ||
	    (IS_ENABLED(CONFIG_ZELOS_UDS_OBD) && group == DTC_GROUP_EMISSIONS)) {
		for (size_t j = 0; j < cfg->dtc_count; j++) {
			uds_clear_record(&uds->memory->dtc[j]);
		}
		return 0;
	}
	i = find_dtc(cfg, group);
	if (i < 0) {
		return NRC_OUT_OF_RANGE;
	}
	uds_clear_record(&uds->memory->dtc[i]);
	return 0;
}

bool uds_put_dtc(struct zelos_uds_core *uds, size_t i, struct rsp *r)
{
	return put_be24(r, uds->config->dtcs[i]) && put_u8(r, uds->memory->dtc[i].status);
}

int uds_snapshot_offset(const struct zelos_uds_config *cfg, uint16_t id)
{
	int n = 0;

	for (size_t i = 0; i < cfg->snapshot_did_count; i++) {
		const struct zelos_uds_did *did = uds_find_did(cfg, cfg->snapshot_dids[i]);

		if (did == NULL) {
			continue;
		}
		if (did->id == id) {
			return n;
		}
		n += did->size;
	}
	return -1;
}

static uint8_t snapshot_length(const struct zelos_uds_config *cfg)
{
	size_t n = 0;

	for (size_t i = 0; i < cfg->snapshot_did_count; i++) {
		const struct zelos_uds_did *did = uds_find_did(cfg, cfg->snapshot_dids[i]);

		n += did != NULL ? did->size : 0;
	}
	return (uint8_t)n;
}

/* Snapshot records, in order: OBD-II first frame, the node's DIDs, OBD-II second frame. */
static const uint8_t records[] = {
#if defined(CONFIG_ZELOS_UDS_OBD)
	UDS_RECORD_FIRST,
#endif
	SNAPSHOT_RECORD,
#if defined(CONFIG_ZELOS_UDS_OBD)
	UDS_RECORD_LATEST,
#endif
};

static bool record_supported(uint8_t record)
{
	for (size_t k = 0; k < ARRAY_SIZE(records); k++) {
		if (records[k] == record) {
			return true;
		}
	}
	return false;
}

/* A snapshot record: number, DID count, then each DID and its value. */
static void put_snapshot(struct zelos_uds_core *uds, const struct zelos_uds_dtc_record *d,
			 uint8_t record, struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	const uint8_t *p = d->snapshot;

#if defined(CONFIG_ZELOS_UDS_OBD)
	if (record != SNAPSHOT_RECORD) {
		uds_obd_put_frame(uds, record == UDS_RECORD_FIRST ? d->snapshot : d->latest, record,
				  r);
		return;
	}
#endif
	put_u8(r, record);
	put_u8(r, (uint8_t)cfg->snapshot_did_count);
	for (size_t j = 0; j < cfg->snapshot_did_count; j++) {
		const struct zelos_uds_did *did = uds_find_did(cfg, cfg->snapshot_dids[j]);

		put_be16(r, did->id);
		put(r, p, did->size);
		p += did->size;
	}
}

static uint8_t read_dtc(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			struct rsp *r, int64_t now)
{
	const struct zelos_uds_config *cfg = uds->config;
	struct zelos_uds_dtc_record *d;
	uint8_t type = req[1];
	int i;

	ARG_UNUSED(now);

	/* No suppressPosRsp on this service: bit 7 set is no sub-function here. */
	switch (type) {
	case 0x01: /* reportNumberOfDTCByStatusMask */
	case 0x02: /* reportDTCByStatusMask */
		if (len != 3) {
			return NRC_INCORRECT_LENGTH;
		}
		put_u8(r, type);
		put_u8(r, DTC_AVAILABILITY);
		if (type == 0x01) {
			uint16_t count = 0;

			for (size_t j = 0; j < cfg->dtc_count; j++) {
				count += (uds->memory->dtc[j].status & req[2]) != 0;
			}
			put_u8(r, DTC_FORMAT_ISO14229);
			put_be16(r, count);
			return 0;
		}
		for (size_t j = 0; j < cfg->dtc_count; j++) {
			if ((uds->memory->dtc[j].status & req[2]) != 0 && !uds_put_dtc(uds, j, r)) {
				return NRC_RESPONSE_TOO_LONG;
			}
		}
		return 0;
	case 0x03: /* reportDTCSnapshotIdentification */
	case 0x0A: /* reportSupportedDTC */
		if (len != 2) {
			return NRC_INCORRECT_LENGTH;
		}
		put_u8(r, type);
		if (type == 0x0A) {
			put_u8(r, DTC_AVAILABILITY);
		}
		for (size_t j = 0; j < cfg->dtc_count; j++) {
			if (type == 0x0A) {
				uds_put_dtc(uds, j, r);
				continue;
			}
			for (size_t k = 0; uds->memory->dtc[j].snapshot_stored && k < ARRAY_SIZE(records);
			     k++) {
				put_be24(r, cfg->dtcs[j]);
				put_u8(r, records[k]);
			}
		}
		return 0;
	case 0x04: /* reportDTCSnapshotRecordByDTCNumber */
	case 0x06: /* reportDTCExtDataRecordByDTCNumber */
		if (len != 6) {
			return NRC_INCORRECT_LENGTH;
		}
		i = find_dtc(cfg, sys_get_be24(&req[2]));
		if (i < 0) {
			return NRC_OUT_OF_RANGE;
		}
		d = &uds->memory->dtc[i];
		if (type == 0x04) {
			if (!record_supported(req[5]) && req[5] != RECORD_ALL) {
				return NRC_OUT_OF_RANGE;
			}
			put_u8(r, type);
			uds_put_dtc(uds, i, r);
			for (size_t k = 0; d->snapshot_stored && k < ARRAY_SIZE(records); k++) {
				if (req[5] == RECORD_ALL || req[5] == records[k]) {
					put_snapshot(uds, d, records[k], r);
				}
			}
			return 0;
		}
		if (req[5] != RECORD_OCCURRENCES && req[5] != RECORD_AGING &&
		    req[5] != RECORD_ALL) {
			return NRC_OUT_OF_RANGE;
		}
		put_u8(r, type);
		uds_put_dtc(uds, i, r);
		if (req[5] != RECORD_AGING) {
			put_u8(r, RECORD_OCCURRENCES);
			put_u8(r, d->occurrences);
		}
		if (req[5] != RECORD_OCCURRENCES) {
			put_u8(r, RECORD_AGING);
			put_u8(r, d->aging);
		}
		return 0;
	default:
#if defined(CONFIG_ZELOS_UDS_OBD)
		return uds_obd_read_dtc(uds, req, len, r);
#else
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
#endif
	}
}

static uint8_t io_control(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			  struct rsp *r, int64_t now)
{
	const struct zelos_uds_config *cfg = uds->config;
	const struct zelos_uds_io *io = NULL;
	uint8_t state[UINT8_MAX];
	uint8_t param, nrc;
	size_t i;

	ARG_UNUSED(now);

	if (len < 4) {
		return NRC_INCORRECT_LENGTH;
	}
	for (i = 0; i < cfg->io_count; i++) {
		if (cfg->ios[i].id == sys_get_be16(&req[1])) {
			io = &cfg->ios[i];
			break;
		}
	}
	param = req[3];
	if (io == NULL || param > ZELOS_UDS_IO_ADJUST) {
		return NRC_OUT_OF_RANGE;
	}
	/* Only a short-term adjustment carries a state. */
	if (len != 4U + (param == ZELOS_UDS_IO_ADJUST ? io->size : 0U)) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->unlocked != ZELOS_UDS_LEVEL_EXTENDED) {
		return NRC_SECURITY_DENIED;
	}
	nrc = io->control(param, param == ZELOS_UDS_IO_ADJUST ? &req[4] : NULL);
	if (nrc != 0) {
		return nrc;
	}
	if (param == ZELOS_UDS_IO_RETURN) {
		uds->io_active &= ~BIT(i);
	} else {
		uds->io_active |= BIT(i);
	}

	io->read(state);
	put(r, &req[1], 3);
	put(r, state, io->size);
	return 0;
}

static uint8_t erase(struct zelos_uds_core *uds, uint8_t type, const uint8_t *opt, size_t n,
		     int64_t now)
{
	uint32_t addr = DOWNLOAD_ADDRESS, size = sizeof(uds->download);

	if (n != 0 && n != 9) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->unlocked != ZELOS_UDS_LEVEL_PROGRAMMING) {
		return NRC_SECURITY_DENIED;
	}
	if (type != 0x01) {
		/* Stop and results: nothing runs between requests. */
		return type == 0x03 && uds->last == ZELOS_UDS_ROUTINE_ERASE ? 0 : NRC_SEQUENCE;
	}
	if (uds->transfer != ZELOS_UDS_TRANSFER_NONE) {
		return NRC_CONDITIONS;
	}
	if (n == 9) {
		addr = sys_get_be32(&opt[1]);
		size = sys_get_be32(&opt[5]);
		if (opt[0] != ALFID_DEFAULT || size == 0 ||
		    !within(DOWNLOAD_ADDRESS, sizeof(uds->download), addr, size)) {
			return NRC_OUT_OF_RANGE;
		}
	}

	uds->erase_addr = addr;
	uds->erase_size = size;
	uds->pending = ZELOS_UDS_ROUTINE_ERASE;
	uds->pending_routine = NULL;
	uds->pending_done = now + ERASE_MS;
	uds->pending_sent = now;
	return NRC_PENDING;
}

static uint8_t check_dependencies(struct zelos_uds_core *uds, uint8_t type, const uint8_t *opt,
				  size_t n)
{
	uint32_t crc;

	if (type == 0x01 && n != 4) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->unlocked != ZELOS_UDS_LEVEL_PROGRAMMING) {
		return NRC_SECURITY_DENIED;
	}
	if (type != 0x01) {
		return type == 0x03 && uds->last == ZELOS_UDS_ROUTINE_CHECK ? 0 : NRC_SEQUENCE;
	}
	if (!uds->downloaded) {
		return NRC_SEQUENCE;
	}

	crc = crc32_ieee(&uds->download[uds->downloaded_addr - DOWNLOAD_ADDRESS],
			 uds->downloaded_size);
	uds->last = ZELOS_UDS_ROUTINE_CHECK;
	uds->last_result[0] = crc == sys_get_be32(opt) ? 0x00 : 0x01;
	sys_put_be32(crc, &uds->last_result[1]);
	uds->last_len = 5;
	return 0;
}

static uint8_t routine_control(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			       struct rsp *r, int64_t now)
{
	const struct zelos_uds_config *cfg = uds->config;
	const struct zelos_uds_routine *routine = NULL;
	uint8_t type = req[1] & ~SUPPRESS;
	uint16_t id;
	uint8_t nrc;

	if (type < 0x01 || type > 0x03) {
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len < 4) {
		return NRC_INCORRECT_LENGTH;
	}
	id = sys_get_be16(&req[2]);

	if (id == ZELOS_UDS_ROUTINE_ERASE || id == ZELOS_UDS_ROUTINE_CHECK) {
		if (uds->session != ZELOS_UDS_SESSION_PROGRAMMING) {
			return NRC_OUT_OF_RANGE;
		}
		if (type != 0x01 && len != 4) {
			return NRC_INCORRECT_LENGTH;
		}
		nrc = id == ZELOS_UDS_ROUTINE_ERASE
			      ? erase(uds, type, &req[4], len - 4, now)
			      : check_dependencies(uds, type, &req[4], len - 4);
		if (nrc != 0) {
			return nrc;
		}
	} else {
		for (size_t i = 0; i < cfg->routine_count; i++) {
			if (cfg->routines[i].id == id) {
				routine = &cfg->routines[i];
				break;
			}
		}
		if (routine == NULL || routine->session != uds->session) {
			return NRC_OUT_OF_RANGE;
		}
		if (len != 4) {
			return NRC_INCORRECT_LENGTH;
		}
		if (type == 0x01) {
			uds->pending = id;
			uds->pending_routine = routine;
			uds->pending_done = now + routine->duration_ms;
			uds->pending_sent = now;
			return NRC_PENDING;
		}
		/* Nothing runs between requests, so nothing to stop. */
		if (type == 0x02 || uds->last != id) {
			return NRC_SEQUENCE;
		}
	}

	put_u8(r, type);
	put_be16(r, id);
	put(r, uds->last_result, uds->last_len);
	return 0;
}

static uint8_t request_transfer(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
				struct rsp *r, int64_t now)
{
	uint8_t addr_len, size_len;
	uint32_t addr, size;
	bool download = req[0] == SID_REQUEST_DOWNLOAD;

	ARG_UNUSED(now);

	/* ISO 14229-1 order: 0x13, 0x31 format, 0x13, 0x33, 0x31 range, 0x22, 0x70. */
	if (len < 3) {
		return NRC_INCORRECT_LENGTH;
	}
	addr_len = req[2] & 0x0F;
	size_len = req[2] >> 4;
	/* Neither compressed nor encrypted. */
	if (req[1] != 0x00 || addr_len < 1 || addr_len > 4 || size_len < 1 || size_len > 4) {
		return NRC_OUT_OF_RANGE;
	}
	if (len != 3U + addr_len + size_len) {
		return NRC_INCORRECT_LENGTH;
	}
	addr = get_be(&req[3], addr_len);
	size = get_be(&req[3 + addr_len], size_len);
	if (uds->unlocked != ZELOS_UDS_LEVEL_PROGRAMMING) {
		return NRC_SECURITY_DENIED;
	}
	if (size == 0 || !within(DOWNLOAD_ADDRESS, sizeof(uds->download), addr, size)) {
		return NRC_OUT_OF_RANGE;
	}
	if (uds->transfer != ZELOS_UDS_TRANSFER_NONE) {
		return NRC_CONDITIONS;
	}
	/* Programming needs memory erased since it was last written. */
	if (download && !within(uds->erased_lo, uds->erased_hi - uds->erased_lo, addr, size)) {
		return NRC_DOWNLOAD_NOT_ACCEPTED;
	}

	if (download) {
		/* Written from here on: no longer erased, no longer checked. */
		uds->erased_lo = uds->erased_hi = 0;
		uds->downloaded = false;
	}
	uds->transfer = download ? ZELOS_UDS_TRANSFER_DOWNLOAD : ZELOS_UDS_TRANSFER_UPLOAD;
	uds->transfer_addr = addr;
	uds->transfer_size = size;
	uds->transfer_done = 0;
	uds->transfer_bsc = 0x01;
	uds->transfer_last_len = 0;
	put_u8(r, LENGTH_FORMAT);
	put_be16(r, CONFIG_ZELOS_UDS_BUF_SIZE);
	return 0;
}

static uint8_t transfer_data(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			     struct rsp *r, int64_t now)
{
	uint8_t *at;
	uint8_t bsc;
	bool repeat;
	size_t n;

	ARG_UNUSED(now);

	if (len < 2) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->transfer == ZELOS_UDS_TRANSFER_NONE) {
		return NRC_SEQUENCE;
	}
	bsc = req[1];
	/* The last block again, its response lost: answered as before. */
	repeat = uds->transfer_done > 0 && bsc == (uint8_t)(uds->transfer_bsc - 1);
	if (!repeat && bsc != uds->transfer_bsc) {
		return NRC_WRONG_BSC;
	}

	at = &uds->download[uds->transfer_addr - DOWNLOAD_ADDRESS];
	if (uds->transfer == ZELOS_UDS_TRANSFER_UPLOAD) {
		if (len != 2) {
			return NRC_INCORRECT_LENGTH;
		}
		if (repeat) {
			n = uds->transfer_last_len;
			at += uds->transfer_done - n;
		} else {
			n = MIN(uds->transfer_size - uds->transfer_done,
				CONFIG_ZELOS_UDS_BUF_SIZE - 2U);
			if (n == 0) {
				return NRC_SEQUENCE;
			}
			at += uds->transfer_done;
			uds->transfer_done += n;
			uds->transfer_last_len = n;
			uds->transfer_bsc++;
		}
		put_u8(r, bsc);
		put(r, at, n);
		return 0;
	}

	n = len - 2;
	if (n == 0) {
		return NRC_INCORRECT_LENGTH;
	}
	if (!repeat) {
		if (n > uds->transfer_size - uds->transfer_done) {
			return NRC_TRANSFER_SUSPENDED;
		}
		memcpy(at + uds->transfer_done, &req[2], n);
		uds->transfer_done += n;
		uds->transfer_bsc++;
	}
	put_u8(r, bsc);
	return 0;
}

static uint8_t transfer_exit(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			     struct rsp *r, int64_t now)
{
	ARG_UNUSED(req);
	ARG_UNUSED(r);
	ARG_UNUSED(now);

	if (len != 1) {
		return NRC_INCORRECT_LENGTH;
	}
	if (uds->transfer == ZELOS_UDS_TRANSFER_NONE ||
	    uds->transfer_done != uds->transfer_size) {
		return NRC_SEQUENCE;
	}

	if (uds->transfer == ZELOS_UDS_TRANSFER_DOWNLOAD) {
		uds->downloaded = true;
		uds->downloaded_addr = uds->transfer_addr;
		uds->downloaded_size = uds->transfer_size;
	}
	uds->transfer = ZELOS_UDS_TRANSFER_NONE;
	return 0;
}

struct service {
	uint8_t sid;
	/* Sessions it is supported in, as BIT(session). */
	uint8_t sessions;
	/* Its first byte is a sub-function with the suppressPosRsp bit. */
	bool subfunction;
	uint8_t (*handle)(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			  struct rsp *r, int64_t now);
};

/* Anything else, Tier B included (0x24, 0x29, 0x2A, 0x2C, 0x38, 0x83, 0x84, 0x86, 0x87): 0x11. */
static const struct service services[] = {
	{SID_SESSION_CONTROL, S_ANY, true, session_control},
	{SID_ECU_RESET, S_ANY, true, ecu_reset},
	{SID_CLEAR_DTC, S_ANY, false, clear_dtc},
	{SID_READ_DTC, S_ANY, false, read_dtc},
	{SID_READ_DID, S_ANY, false, read_did},
	{SID_READ_MEMORY, S_ANY, false, read_memory},
	{SID_SECURITY_ACCESS, S_NON_DEFAULT, true, security_access},
	{SID_COMMUNICATION_CONTROL, S_NON_DEFAULT, true, communication_control},
	{SID_WRITE_DID, S_NON_DEFAULT, false, write_did},
	{SID_IO_CONTROL, S_EXTENDED, false, io_control},
	{SID_ROUTINE_CONTROL, S_NON_DEFAULT, true, routine_control},
	{SID_REQUEST_DOWNLOAD, S_PROGRAMMING, false, request_transfer},
	{SID_REQUEST_UPLOAD, S_PROGRAMMING, false, request_transfer},
	{SID_TRANSFER_DATA, S_PROGRAMMING, false, transfer_data},
	{SID_TRANSFER_EXIT, S_PROGRAMMING, false, transfer_exit},
	{SID_WRITE_MEMORY, S_EXTENDED, false, write_memory},
	{SID_TESTER_PRESENT, S_ANY, true, tester_present},
	{SID_DTC_SETTING, S_NON_DEFAULT, true, dtc_setting},
};

/* ISO 14229-1 7.5: not sent in answer to a functionally addressed request. */
static bool functional_silent(uint8_t nrc)
{
	return nrc == NRC_SERVICE_NOT_SUPPORTED || nrc == NRC_SUBFUNCTION_NOT_SUPPORTED ||
	       nrc == NRC_OUT_OF_RANGE || nrc == NRC_SUBFUNCTION_NOT_IN_SESSION ||
	       nrc == NRC_SERVICE_NOT_IN_SESSION;
}

static const struct service *find_service(uint8_t sid)
{
	for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
		if (services[i].sid == sid) {
			return &services[i];
		}
	}
	return NULL;
}

static uint8_t dispatch(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			struct rsp *r, int64_t now)
{
	const struct service *s = find_service(req[0]);

	if (s == NULL) {
		return NRC_SERVICE_NOT_SUPPORTED;
	}
	if ((s->sessions & BIT(uds->session)) == 0) {
		return NRC_SERVICE_NOT_IN_SESSION;
	}
	if (len < 2 && (s->subfunction || s->sid == SID_READ_DTC)) {
		return NRC_INCORRECT_LENGTH;
	}
	if (s->subfunction) {
		r->suppress = (req[1] & SUPPRESS) != 0;
	}
	return s->handle(uds, req, len, r, now);
}

static uint32_t memory_crc(const struct zelos_uds_dtc_memory *m)
{
	return crc32_ieee((const uint8_t *)&m->stored,
			  offsetof(struct zelos_uds_dtc_memory, crc) -
				  offsetof(struct zelos_uds_dtc_memory, stored));
}

/*
 * The end of an operation cycle, at the start of the next: Annex D, then the
 * OBD-II lifecycle (CCR 1968.2 (d)(2.2)-(2.5), see include/zelos/uds.h).
 */
static void start_cycle(struct zelos_uds_dtc_record *d)
{
	bool tested = (d->status & TNCTOC) == 0;
	bool failed = (d->status & TFTOC) != 0;

	if (!failed && (d->status & (CDTC | WIR)) == CDTC && ++d->aging >= AGING_CYCLES) {
		/* (d)(2.4): AGING_CYCLES warm-up cycles with the MIL off; frames go with it. */
		d->status &= ~CDTC;
		d->aging = 0;
		d->snapshot_stored = false;
	}
	if (tested && !failed) {
		d->status &= ~PDTC;
		d->failed_cycles = 0;
		if ((d->status & CDTC) == 0) {
			/* (d)(2.2.7)(B)(i): a pending code erased takes its frames. */
			d->snapshot_stored = false;
		}
		if ((d->status & WIR) == 0) {
			/* (d)(2.5.2)(A): MIL off (a clear, a power loss), one passed cycle. */
			d->permanent = false;
		} else if (++d->healing >= HEALING_CYCLES) {
			/* (d)(2.3.2), (d)(2.5.1): the MIL goes off, the permanent code with it. */
			d->status &= ~WIR;
			d->healing = 0;
			d->permanent = false;
		}
	}
	d->status &= ~TFTOC;
	d->status |= TNCTOC;
}

int zelos_uds_core_init(struct zelos_uds_core *uds, const struct zelos_uds_config *config,
			uint32_t (*random)(void), struct zelos_uds_dtc_memory *memory,
			uint32_t permanent, int64_t now)
{
	bool kept;

	if (config->dtc_count > ZELOS_UDS_DTCS_MAX || config->io_count > 32 ||
	    snapshot_length(config) > ZELOS_UDS_SNAPSHOT_MAX) {
		return -EINVAL;
	}
	/* Every PID reads a DID of the node's, into at most 4 bytes. */
	for (size_t i = 0; i < config->pid_count; i++) {
		if (config->pids[i].size > 4 || uds_find_did(config, config->pids[i].did) == NULL) {
			return -EINVAL;
		}
	}

	memset(uds, 0, sizeof(*uds));
	uds->config = config;
	uds->random = random;
	uds->memory = memory;
	uds->session = ZELOS_UDS_SESSION_DEFAULT;
	uds->s3_deadline = now + S3_MS;
	uds->dtc_setting = true;
	/* A tester must wait out the delay after power-up too. */
	uds->lockout_until[0] = now + SECURITY_DELAY_MS;
	uds->lockout_until[1] = now + SECURITY_DELAY_MS;

	kept = memory->magic == MEMORY_MAGIC && memory->crc == memory_crc(memory);
	memory->magic = 0;
	if (!kept) {
		memory->stored = 0;
	}
	for (size_t i = 0; i < config->dtc_count; i++) {
		if (kept) {
			start_cycle(&memory->dtc[i]);
		} else {
			/* A power loss: a clear (CCR (d)(2.5.2)), permanent DTCs from flash. */
			erase_record(&memory->dtc[i]);
			memory->dtc[i].permanent = (permanent & BIT(i)) != 0;
		}
	}
	return 0;
}

static size_t negative(uint8_t *rsp, uint8_t sid, uint8_t nrc)
{
	rsp[0] = NEGATIVE;
	rsp[1] = sid;
	rsp[2] = nrc;
	return 3;
}

size_t zelos_uds_core_request(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			      bool functional, int64_t now, uint8_t *rsp)
{
	struct rsp r = {.buf = rsp, .len = 1};
	bool obd = false;
	uint8_t nrc;

	if (len == 0) {
		return 0;
	}

	/* Every request restarts S3, answered or not. */
	uds->s3_deadline = now + S3_MS;

#if defined(CONFIG_ZELOS_UDS_OBD)
	/* J1979 services: any session, never negative, never busy. */
	if (req[0] >= 0x01 && req[0] <= 0x0A) {
		return uds_obd_request(uds, req, len, rsp);
	}
	obd = uds_obd_relevant(req, len);
#endif

	if (uds->pending != 0 && !obd) {
		const struct service *s = find_service(req[0]);

		/* A keep-alive needs no answer; anything else must wait. */
		if (len == 2 && req[0] == SID_TESTER_PRESENT && req[1] == SUPPRESS) {
			return 0;
		}
		/*
		 * A functional request gets no 0x21 where an idle server would not
		 * answer (0x11, 0x7F). suppressPosRsp hides positive responses only.
		 */
		if (functional && (s == NULL || (s->sessions & BIT(uds->session)) == 0)) {
			return 0;
		}
		return negative(rsp, req[0], NRC_BUSY_REPEAT);
	}

	rsp[0] = req[0] + POSITIVE;
	nrc = dispatch(uds, req, len, &r, now);
	if (nrc == NRC_PENDING) {
		/* The final response follows from zelos_uds_core_tick(), suppressed or not. */
		return negative(rsp, req[0], nrc);
	}
	if (nrc != 0) {
		/* CCR 1968.2 (g)(3.4.2)(A): an OBD request is never answered 0x13. */
		if ((functional && functional_silent(nrc)) || (obd && nrc == NRC_INCORRECT_LENGTH)) {
			return 0;
		}
		return negative(rsp, req[0], nrc);
	}
	return r.suppress ? 0 : r.len;
}

static void finish_pending(struct zelos_uds_core *uds)
{
	if (uds->pending == ZELOS_UDS_ROUTINE_ERASE) {
		memset(&uds->download[uds->erase_addr - DOWNLOAD_ADDRESS], 0xFF, uds->erase_size);
		uds->erased_lo = uds->erase_addr;
		uds->erased_hi = uds->erase_addr + uds->erase_size;
		uds->downloaded = false;
		uds->last_result[0] = 0x00;
		uds->last_len = 1;
	} else {
		uds->last_len = uds->pending_routine->run(uds->last_result);
	}
	uds->last = uds->pending;
	uds->pending = 0;
}

size_t zelos_uds_core_tick(struct zelos_uds_core *uds, int64_t now, uint8_t *rsp)
{
	if (uds->pending != 0) {
		struct rsp r = {.buf = rsp, .len = 0};

		if (now >= uds->pending_done) {
			finish_pending(uds);
			/* S3 runs again from the final response. */
			uds->s3_deadline = now + S3_MS;
			put_u8(&r, SID_ROUTINE_CONTROL + POSITIVE);
			put_u8(&r, 0x01);
			put_be16(&r, uds->last);
			put(&r, uds->last_result, uds->last_len);
			return r.len;
		}
		if (now - uds->pending_sent >= PENDING_REPEAT_MS) {
			uds->pending_sent = now;
			return negative(rsp, SID_ROUTINE_CONTROL, NRC_PENDING);
		}
		return 0;
	}

	if (uds->session != ZELOS_UDS_SESSION_DEFAULT && now >= uds->s3_deadline) {
		enter_session(uds, ZELOS_UDS_SESSION_DEFAULT);
	}
	return 0;
}

static void capture_snapshot(struct zelos_uds_core *uds, uint8_t *p)
{
	const struct zelos_uds_config *cfg = uds->config;

	for (size_t j = 0; j < cfg->snapshot_did_count; j++) {
		const struct zelos_uds_did *did = uds_find_did(cfg, cfg->snapshot_dids[j]);

		did->read(p);
		p += did->size;
	}
}

void zelos_uds_core_dtc(struct zelos_uds_core *uds, size_t index, bool failed)
{
	struct zelos_uds_dtc_record *d;
	bool first;

	if (index >= uds->config->dtc_count || !uds->dtc_setting) {
		return;
	}
	d = &uds->memory->dtc[index];
	if (!failed) {
		d->status &= ~(TF | TNCTOC | TNCSLC);
		return;
	}
	first = (d->status & TFTOC) == 0;
	if (first && d->occurrences < UINT8_MAX) {
		d->occurrences++;
	}
	/* Both frames with the pending code; the second again in each later cycle. */
	if (!d->snapshot_stored) {
		capture_snapshot(uds, d->snapshot);
		memcpy(d->latest, d->snapshot, sizeof(d->latest));
		d->snapshot_stored = true;
		d->stored_at = ++uds->memory->stored;
	} else if (first) {
		capture_snapshot(uds, d->latest);
	}
	d->status |= TF | TFTOC | PDTC | TFSLC;
	d->status &= ~(TNCTOC | TNCSLC);
	d->aging = 0;
	d->healing = 0;
	if (first && d->failed_cycles < UINT8_MAX) {
		d->failed_cycles++;
	}
	if (d->failed_cycles >= CONFIRM_CYCLES) {
		/* Confirmed, the MIL on, and permanent while it is (CCR (d)(2.2.5)). */
		d->status |= CDTC | WIR;
		d->permanent = true;
	}
}

uint32_t zelos_uds_core_permanent(const struct zelos_uds_core *uds)
{
	uint32_t mask = 0;

	for (size_t i = 0; i < uds->config->dtc_count; i++) {
		mask |= uds->memory->dtc[i].permanent ? BIT(i) : 0;
	}
	return mask;
}

void zelos_uds_core_persist(struct zelos_uds_core *uds)
{
	uds->memory->crc = memory_crc(uds->memory);
	uds->memory->magic = MEMORY_MAGIC;
}
