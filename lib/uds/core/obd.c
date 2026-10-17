/*
 * OBD-II on the UDS core: SAE J1979 services 0x01-0x0A, and SAE J1979-2
 * (OBDonUDS) as DIDs and ReadDTCInformation sub-functions. Same DTC memory,
 * same DIDs, same lock as the rest of the server. See include/zelos/uds.h.
 */

#include "core.h"

#include <zephyr/sys/crc.h>

enum {
	SID_CURRENT_DATA = 0x01,
	SID_FREEZE_FRAME = 0x02,
	SID_CONFIRMED_DTCS = 0x03,
	SID_CLEAR = 0x04,
	SID_PENDING_DTCS = 0x07,
	SID_VEHICLE_INFO = 0x09,
	SID_PERMANENT_DTCS = 0x0A,
};

#define POSITIVE 0x40

/* PIDs the core answers itself: monitor status, OBD standard. */
#define PID_MONITOR_STATUS 0x01
#define PID_OBD_STANDARD 0x1C
/* Service 0x02's PID 0x02: the DTC that stored the frame. */
#define PID_FREEZE_DTC 0x02
#define OBD_STANDARD_OBDII_CARB 0x01
#define MIL BIT(7)
/* Infotypes (service 0x09). */
#define INFO_VIN 0x02
#define INFO_CALID 0x04
#define INFO_CVN 0x06
#define INFO_ECU_NAME 0x0A
#define DID_VIN 0xF190
#define VIN_LEN 17
#define CVN_LEN 4

/* J1979-2 DID ranges, DID = base + PID or infotype. */
#define DID_PIDS 0xF400
#define DID_MONITOR 0xF500
#define DID_INFO 0xF800
#define DID_MIL_COUNT 0xF501
/* ISO 15765-4:2021 6.1.1: OBDonUDS protocol identification, 0x01 J1979-2. */
#define DID_PROTOCOL 0xF810
#define PROTOCOL_J1979_2 0x01
#define DID_PIDS_HI 0xF4
#define DID_INFO_HI 0xF8
#define DID_DTR_HI 0xE0

/* ReadDTCInformation 0x42, 0x55, 0x56 (ISO 14229-1:2020). */
#define REPORT_WWH_BY_MASK 0x42
#define REPORT_WWH_PERMANENT 0x55
#define REPORT_WWH_READINESS 0x56
#define GROUP_EMISSIONS 0x33
/* SAE J2012-DA DTCFormat_04: 3-byte DTCs, as J1979-2 reports them. */
#define DTC_FORMAT_J2012_DA_04 0x04
/* DTC severity class 1 (bits 0-4, GTR 5 class A), as testers ask with mask 0x02. Convention. */
#define SEVERITY_CLASS_1 0x02
#define NRC_SUBFUNCTION_NOT_SUPPORTED 0x12
#define SID_CLEAR_DTC 0x14
#define SID_READ_DTC 0x19
#define SID_READ_DID 0x22
#define REPORT_SNAPSHOT 0x04
/* J1979-2 per-DTC extended data: 0x91 in-use ratio, 0x92 test results (CCR (d)(5.1), (g)(4.5.1)). */
#define REPORT_EXT_DATA 0x06
#define REPORT_SUPPORTED_EXT_DATA 0x1A
#define RECORD_IUMPR 0x91
#define RECORD_TEST_RESULTS 0x92
#define GROUP_EMISSIONS_24 0xFFFF33
/* Legacy request limits on CAN: 6 PIDs, 3 freeze frame pairs, one data infotype. */
#define MAX_PIDS 6
#define MAX_FRAME_PAIRS 3
#define MAX_INFOTYPES 6
/* PID 0x01 byte B: comprehensive components supported (bit 2), complete (bit 6 clear). */
#define CCM_SUPPORTED BIT(2)

static const struct zelos_uds_pid *find_pid(const struct zelos_uds_config *cfg, uint8_t pid)
{
	for (size_t i = 0; i < cfg->pid_count; i++) {
		if (cfg->pids[i].pid == pid) {
			return &cfg->pids[i];
		}
	}
	return NULL;
}

/* How a PID (or infotype) is supported: live, in the freeze frame, or at all. */
enum scope { SCOPE_LIVE, SCOPE_FROZEN, SCOPE_INFO };

static bool supported(const struct zelos_uds_config *cfg, uint8_t id, enum scope scope)
{
	const struct zelos_uds_pid *p;

	switch (scope) {
	case SCOPE_INFO:
		return (IS_ENABLED(CONFIG_ZELOS_UDS_OBD_VIN) && id == INFO_VIN) || id == INFO_CALID ||
		       id == INFO_CVN || id == INFO_ECU_NAME;
	case SCOPE_FROZEN:
		if (id == PID_FREEZE_DTC) {
			return true;
		}
		p = find_pid(cfg, id);
		return p != NULL && uds_snapshot_offset(cfg, p->did) >= 0;
	default:
		return id == PID_MONITOR_STATUS || id == PID_OBD_STANDARD ||
		       find_pid(cfg, id) != NULL;
	}
}

/*
 * The 32-bit support bitmap of ids base + 1 .. base + 32, MSB first; the last
 * bit says whether the next bitmap has anything.
 */
static uint32_t bitmap(const struct zelos_uds_config *cfg, uint8_t base, enum scope scope)
{
	uint32_t bits = 0;

	for (unsigned int id = base + 1; id <= 0xFF; id++) {
		if (supported(cfg, (uint8_t)id, scope)) {
			bits |= id - base <= 32 ? BIT(32 - (id - base)) : BIT(0);
		}
	}
	return bits;
}

static bool is_bitmap(uint8_t id)
{
	return (id & 0x1F) == 0;
}

/*
 * MIL (any warningIndicatorRequested), the confirmed count, and the
 * comprehensive component monitor (every DTC's test), always complete.
 */
static void monitor_status(struct zelos_uds_core *uds, uint8_t *out)
{
	uint8_t count = 0;
	bool mil = false;

	for (size_t i = 0; i < uds->config->dtc_count; i++) {
		count += (uds->memory->dtc[i].status & CDTC) != 0;
		mil |= (uds->memory->dtc[i].status & WIR) != 0;
	}
	out[0] = (mil ? MIL : 0) | MIN(count, 0x7F);
	out[1] = uds->config->dtc_count > 0 ? CCM_SUPPORTED : 0;
	out[2] = out[3] = 0;
}

/* The DTC whose frame was stored first: freeze frame 0's, or -1 (CCR (g)(4.3.1)(D)). */
static int frozen_dtc(struct zelos_uds_core *uds)
{
	int owner = -1;

	for (size_t i = 0; i < uds->config->dtc_count; i++) {
		const struct zelos_uds_dtc_record *d = &uds->memory->dtc[i];

		if (d->snapshot_stored &&
		    (owner < 0 || d->stored_at < uds->memory->dtc[owner].stored_at)) {
			owner = (int)i;
		}
	}
	return owner;
}

/* Availability ids and data ids in one request: the tester's error, ignored (SWS_Dcm_00943/00972). */
static bool mixed(const uint8_t *ids, size_t n, size_t step)
{
	size_t bitmaps = 0, items = 0;

	for (size_t i = 0; i < n; i += step) {
		bitmaps += is_bitmap(ids[i]);
		items++;
	}
	return bitmaps != 0 && bitmaps < items;
}

/* A PID's bytes from its DID's current value, or from a snapshot when given. */
static bool put_pid(struct zelos_uds_core *uds, uint8_t pid, const uint8_t *snapshot,
		    struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	const struct zelos_uds_pid *p = find_pid(cfg, pid);
	uint8_t value[UINT8_MAX];
	uint8_t out[4];

	if (snapshot == NULL) {
		if (pid == PID_MONITOR_STATUS) {
			monitor_status(uds, out);
			return put(r, out, 4);
		}
		if (pid == PID_OBD_STANDARD) {
			return put_u8(r, OBD_STANDARD_OBDII_CARB);
		}
	}
	if (p == NULL) {
		return false;
	}
	if (snapshot == NULL) {
		uds_find_did(cfg, p->did)->read(value);
		p->encode(value, out);
	} else {
		int at = uds_snapshot_offset(cfg, p->did);

		if (at < 0) {
			return false;
		}
		p->encode(&snapshot[at], out);
	}
	return put(r, out, p->size);
}

/* Infotype data without its count: VIN, CALID, CVN, ECU name. */
static bool put_info(struct zelos_uds_core *uds, uint8_t info, struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	const struct zelos_uds_did *vin;
	uint8_t text[ZELOS_UDS_ECU_NAME_LEN] = {0};
	const char *s;
	size_t n;

	switch (info) {
	case INFO_VIN:
		vin = uds_find_did(cfg, DID_VIN);
		if (vin == NULL || vin->size != VIN_LEN) {
			return false;
		}
		vin->read(text);
		return put(r, text, VIN_LEN);
	case INFO_CVN:
		/* The download window's CRC-32, the value the check routine computes. */
		if (uds->cvn_stale) {
			uds->cvn = crc32_ieee(uds->download, sizeof(uds->download));
			uds->cvn_stale = false;
		}
		return put_be32(r, uds->cvn);
	case INFO_CALID:
	case INFO_ECU_NAME:
		s = info == INFO_CALID ? cfg->calid : cfg->ecu_name;
		n = info == INFO_CALID ? ZELOS_UDS_CALID_LEN : ZELOS_UDS_ECU_NAME_LEN;
		if (s != NULL) {
			memcpy(text, s, MIN(strlen(s), n));
		}
		return put(r, text, n);
	default:
		return false;
	}
}

/* 0x01: `PID data` per supported PID, bitmaps included; nothing if none is. */
static bool current_data(struct zelos_uds_core *uds, const uint8_t *pids, size_t n, struct rsp *r)
{
	bool any = false;

	if (n > MAX_PIDS || mixed(pids, n, 1)) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (is_bitmap(pids[i])) {
			uint32_t bits = bitmap(uds->config, pids[i], SCOPE_LIVE);

			if (bits != 0 || pids[i] == 0) {
				any |= put_u8(r, pids[i]) && put_be32(r, bits);
			}
		} else if (supported(uds->config, pids[i], SCOPE_LIVE)) {
			any |= put_u8(r, pids[i]) && put_pid(uds, pids[i], NULL, r);
		}
	}
	return any;
}

/* 0x02: `PID frame data` per supported PID of frame 0. */
static bool freeze_frame(struct zelos_uds_core *uds, const uint8_t *req, size_t n, struct rsp *r)
{
	int dtc = frozen_dtc(uds);
	bool any = false;

	if (n % 2 != 0 || n / 2 > MAX_FRAME_PAIRS || mixed(req, n, 2)) {
		return false;
	}
	for (size_t i = 0; i + 1 < n; i += 2) {
		uint8_t pid = req[i], frame = req[i + 1];

		if (frame != 0) {
			continue;
		}
		if (is_bitmap(pid)) {
			uint32_t bits = bitmap(uds->config, pid, SCOPE_FROZEN);

			if (bits != 0 || pid == 0) {
				any |= put_u8(r, pid) && put_u8(r, 0) && put_be32(r, bits);
			}
		} else if (pid == PID_FREEZE_DTC) {
			any |= put_u8(r, pid) && put_u8(r, 0) &&
			       put_be16(r, dtc < 0 ? 0 : uds->config->dtcs[dtc] >> 8);
		} else if (dtc >= 0 && supported(uds->config, pid, SCOPE_FROZEN)) {
			any |= put_u8(r, pid) && put_u8(r, 0) &&
			       put_pid(uds, pid, uds->memory->dtc[dtc].snapshot, r);
		}
	}
	return any;
}

static bool dtc_listed(const struct zelos_uds_dtc_record *d, uint8_t sid)
{
	switch (sid) {
	case SID_CONFIRMED_DTCS:
		return (d->status & CDTC) != 0;
	case SID_PENDING_DTCS:
		return (d->status & PDTC) != 0;
	default:
		return d->permanent;
	}
}

/* 0x03, 0x07, 0x0A: count, then each DTC's first two bytes. */
static void dtc_list(struct zelos_uds_core *uds, uint8_t sid, struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	size_t count = 0;

	put_u8(r, 0);
	for (size_t i = 0; i < cfg->dtc_count; i++) {
		if (dtc_listed(&uds->memory->dtc[i], sid)) {
			put_be16(r, cfg->dtcs[i] >> 8);
			count++;
		}
	}
	r->buf[1] = (uint8_t)count;
}

/* 0x09: `infotype count data` for one data infotype, or up to 6 bitmaps. */
static bool vehicle_info(struct zelos_uds_core *uds, const uint8_t *info, size_t n, struct rsp *r)
{
	bool any = false;

	if (n == 1 && !is_bitmap(info[0])) {
		return supported(uds->config, info[0], SCOPE_INFO) && put_u8(r, info[0]) &&
		       put_u8(r, 1) && put_info(uds, info[0], r);
	}
	if (n > MAX_INFOTYPES) {
		return false;
	}
	/* Bitmaps only: a data infotype among them is ignored with the rest. */
	for (size_t i = 0; i < n; i++) {
		uint32_t bits;

		if (!is_bitmap(info[i])) {
			return false;
		}
		bits = bitmap(uds->config, info[i], SCOPE_INFO);
		if (bits != 0 || info[i] == 0) {
			any |= put_u8(r, info[i]) && put_be32(r, bits);
		}
	}
	return any;
}

size_t uds_obd_request(struct zelos_uds_core *uds, const uint8_t *req, size_t len, uint8_t *rsp)
{
	struct rsp r = {.buf = rsp, .len = 1};
	bool ok;

	rsp[0] = req[0] + POSITIVE;
	switch (req[0]) {
	case SID_CURRENT_DATA:
		ok = len >= 2 && current_data(uds, &req[1], len - 1, &r);
		break;
	case SID_FREEZE_FRAME:
		ok = len >= 3 && freeze_frame(uds, &req[1], len - 1, &r);
		break;
	case SID_CONFIRMED_DTCS:
	case SID_PENDING_DTCS:
	case SID_PERMANENT_DTCS:
		ok = len == 1;
		if (ok) {
			dtc_list(uds, req[0], &r);
		}
		break;
	case SID_CLEAR:
		ok = len == 1;
		for (size_t i = 0; ok && i < uds->config->dtc_count; i++) {
			uds_clear_record(&uds->memory->dtc[i]);
		}
		break;
	case SID_VEHICLE_INFO:
		ok = len >= 2 && vehicle_info(uds, &req[1], len - 1, &r);
		break;
	default:
		ok = false;
		break;
	}
	return ok ? r.len : 0;
}

bool uds_obd_read_did(struct zelos_uds_core *uds, uint16_t id, struct rsp *r, bool *found)
{
	uint8_t id_lo = (uint8_t)id;
	uint16_t base = id & 0xFF00;
	uint8_t out[4];

	*found = true;
	if (id == DID_PROTOCOL) {
		return put_be16(r, id) && put_u8(r, PROTOCOL_J1979_2);
	}
	if (base == DID_PIDS || base == DID_INFO) {
		enum scope sc = base == DID_PIDS ? SCOPE_LIVE : SCOPE_INFO;

		if (is_bitmap(id_lo)) {
			uint32_t bits = bitmap(uds->config, id_lo, sc);

			if (bits != 0 || id_lo == 0) {
				return put_be16(r, id) && put_be32(r, bits);
			}
		} else if (supported(uds->config, id_lo, sc)) {
			return put_be16(r, id) && (sc == SCOPE_LIVE ? put_pid(uds, id_lo, NULL, r)
								    : put_info(uds, id_lo, r));
		}
	} else if (id == DID_MONITOR) {
		return put_be16(r, id) && put_be32(r, BIT(31));
	} else if (id == DID_MIL_COUNT) {
		monitor_status(uds, out);
		return put_be16(r, id) && put(r, out, 4);
	}
	*found = false;
	return true;
}

/*
 * 0x42: severity, DTC, status of each DTC both masks select; 0x55: DTC and
 * status of each permanent DTC (DTCAndStatusRecord, SWS_Dcm_01659).
 */
static bool put_wwh_records(struct zelos_uds_core *uds, const uint8_t *req, struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	bool permanent = req[1] == REPORT_WWH_PERMANENT;

	for (size_t i = 0; i < cfg->dtc_count; i++) {
		const struct zelos_uds_dtc_record *d = &uds->memory->dtc[i];
		bool selected = permanent ? d->permanent
					  : (d->status & req[3]) != 0 && (SEVERITY_CLASS_1 & req[4]) != 0;

		if (selected && !((permanent || put_u8(r, SEVERITY_CLASS_1)) && uds_put_dtc(uds, i, r))) {
			return false;
		}
	}
	return true;
}

uint8_t uds_obd_read_dtc(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			 struct rsp *r)
{
	uint8_t type = req[1];
	size_t want;

	switch (type) {
	case REPORT_WWH_BY_MASK:
		want = 5;
		break;
	case REPORT_WWH_PERMANENT:
		want = 3;
		break;
	case REPORT_WWH_READINESS:
		want = 4;
		break;
	case REPORT_SUPPORTED_EXT_DATA:
		/*
		 * No DTC of this node has a ratio or test-result monitor (comprehensive
		 * components only), so none supports 0x91 / 0x92 (SWS_Dcm_01609).
		 */
		if (len < 3 || (req[2] != RECORD_IUMPR && req[2] != RECORD_TEST_RESULTS)) {
			return NRC_SUBFUNCTION_NOT_SUPPORTED;
		}
		return len == 3 ? NRC_OUT_OF_RANGE : NRC_INCORRECT_LENGTH;
	default:
		return NRC_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (len != want) {
		return NRC_INCORRECT_LENGTH;
	}
	/* No DTC of this node belongs to a readiness group (SWS_Dcm_01612). */
	if (req[2] != GROUP_EMISSIONS || type == REPORT_WWH_READINESS) {
		return NRC_OUT_OF_RANGE;
	}
	put_u8(r, type);
	put_u8(r, GROUP_EMISSIONS);
	put_u8(r, DTC_AVAILABILITY);
	if (type == REPORT_WWH_BY_MASK) {
		put_u8(r, SEVERITY_CLASS_1);
	}
	put_u8(r, DTC_FORMAT_J2012_DA_04);
	return put_wwh_records(uds, req, r) ? 0 : NRC_RESPONSE_TOO_LONG;
}

bool uds_obd_relevant(const uint8_t *req, size_t len)
{
	switch (req[0]) {
	case SID_READ_DID:
		return len >= 2 && ((req[1] >= DID_PIDS_HI && req[1] <= DID_INFO_HI) ||
				    req[1] == DID_DTR_HI);
	case SID_READ_DTC:
		return len >= 2 &&
		       (req[1] == REPORT_WWH_BY_MASK || req[1] == REPORT_WWH_PERMANENT ||
			req[1] == REPORT_WWH_READINESS ||
			(req[1] == REPORT_SNAPSHOT && len == 6 &&
			 (req[5] == UDS_RECORD_FIRST || req[5] == UDS_RECORD_LATEST)) ||
			(req[1] == REPORT_EXT_DATA && len == 6 &&
			 (req[5] == RECORD_IUMPR || req[5] == RECORD_TEST_RESULTS)) ||
			(req[1] == REPORT_SUPPORTED_EXT_DATA && len >= 3 &&
			 (req[2] == RECORD_IUMPR || req[2] == RECORD_TEST_RESULTS)));
	case SID_CLEAR_DTC:
		return len >= 4 && sys_get_be24(&req[1]) == GROUP_EMISSIONS_24;
	default:
		return false;
	}
}

void uds_obd_put_frame(struct zelos_uds_core *uds, const uint8_t *snapshot, uint8_t record,
		       struct rsp *r)
{
	const struct zelos_uds_config *cfg = uds->config;
	size_t at = r->len + 1;
	uint8_t count = 0;

	put_u8(r, record);
	put_u8(r, 0);
	for (size_t i = 0; i < cfg->pid_count; i++) {
		uint8_t pid = cfg->pids[i].pid;

		if (supported(cfg, pid, SCOPE_FROZEN) && put_be16(r, DID_PIDS + pid) &&
		    put_pid(uds, pid, snapshot, r)) {
			count++;
		}
	}
	if (at < CONFIG_ZELOS_UDS_BUF_SIZE) {
		r->buf[at] = count;
	}
}
