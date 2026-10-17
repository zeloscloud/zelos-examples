/*
 * UDS (ISO 14229-1:2020) server on ISO-TP (ISO 15765-2).
 *
 * A diagnostic server as an OEM baseline ECU carries one: sessions, two
 * security levels, communication and DTC-setting control, data identifiers,
 * memory by address, DTCs with ISO 14229-1 Annex D status, I/O control,
 * routines, ECU reset, and download and upload of a RAM image. Requests arrive
 * physically addressed on one identifier and functionally addressed on
 * another; responses leave on a third.
 *
 * Sessions: default (0x01), programming (0x02, from extended or programming
 * only, else NRC 0x22), extended (0x03). Every DiagnosticSessionControl, and
 * S3 (5 s without a request outside default), locks security, returns every
 * output to the ECU and ends a transfer; entering default also re-enables
 * communication (0x28) and DTC setting (0x85).
 *
 * Security: level 0x01 in extended, level 0x11 in programming. The seed is 4
 * random, nonzero bytes; the key is the seed XOR the level's constant below,
 * big-endian. A third wrong key at a level answers 0x36 and that level's seeds
 * 0x37 for 10 s; the delay is also armed for 10 s at power-up.
 *
 * DTC memory survives ECUReset (it lives in RAM the startup code does not
 * clear) and is lost at power-off. Each boot is a new operation cycle. A DTC
 * confirmed with its warning indicator requested is permanent: ClearDTC and
 * OBD service 0x04 leave it, and it goes once its test has passed in three
 * consecutive operation cycles.
 *
 * With CONFIG_ZELOS_UDS_OBD the server is also an OBD-II responder (SAE
 * J1979 / ISO 15031-5) on the same identifiers: services 0x01 (PIDs 0x00,
 * 0x01, 0x1C and the node's), 0x02 (freeze frame 0 = snapshot record 0x01
 * of the first confirmed DTC), 0x03/0x07/0x0A (confirmed, pending, permanent
 * DTCs, 2 bytes each), 0x04 (= ClearDTC of every DTC) and 0x09 (0x02 VIN
 * from DID 0xF190, 0x04 CALID, 0x06 CVN, 0x0A ECU name). J1979 never
 * answers negatively: an unsupported service, PID or infotype gets nothing,
 * physically or functionally. OBDonUDS (SAE J1979-2): DIDs 0xF400 + PID with
 * the PID's bytes (0xF400/0xF420/0xF440 bitmaps), 0xF501 = PID 0x01, 0xF802
 * VIN, 0xF804 CALID, 0xF806 CVN, 0xF80A ECU name (17/16/4/20 bytes, no
 * count); ReadDTCInformation 0x42 (functional group 0x33, status mask,
 * severity 0x00 and ignored, DTC format 0x04), 0x55 (permanent, same
 * records) and 0x56 (readiness group echoed, no DTCs: no monitors);
 * ClearDTC group 0xFFFF33 clears every DTC, as 0xFFFFFF does.
 *
 * NRC order: 0x11, 0x7F, 0x13 (minimum length), 0x12, 0x7E, 0x13, 0x22, 0x24,
 * 0x31 (identifier, address, parameter), 0x33, then data value checks (0x31)
 * and 0x70. RoutineControl after the routine identifier: 0x33, 0x24, 0x22,
 * then the option record (0x31). RequestDownload/Upload: 0x13 (minimum
 * length), 0x31 (formats), 0x13, 0x33, 0x31 (memory range), 0x22, 0x70.
 * 0x21 while a 0x78 is outstanding, except to `3E 80` and to a functional
 * request that is unsupported or not in the session: those get nothing. A
 * functionally addressed request is never answered 0x11, 0x12, 0x31, 0x7E or
 * 0x7F. Functional requests are single frames (ISO 15765-2); a longer one is
 * ignored.
 *
 * Classic CAN, 11-bit identifiers, normal addressing. Half-duplex: a physical
 * request during a multi-frame response aborts the response and is lost.
 * Zephyr's ISO-TP drops a single frame that arrives before the previous one
 * has left its workqueue: requests sent back to back can lose the second.
 */

#ifndef ZELOS_UDS_H_
#define ZELOS_UDS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Security levels, as requestSeed sub-functions, and their keys. */
#define ZELOS_UDS_LEVEL_EXTENDED 0x01
#define ZELOS_UDS_LEVEL_PROGRAMMING 0x11
#define ZELOS_UDS_KEY_XOR_EXTENDED 0x5A5AA5A5U
#define ZELOS_UDS_KEY_XOR_PROGRAMMING 0xC3A53C5AU

/* Read by every tester: the active session, 1 byte. */
#define ZELOS_UDS_DID_ACTIVE_SESSION 0xF186

/*
 * eraseMemory, programming session and level 0x11. Option record: none (the
 * whole download window) or ALFID 0x44, address and size inside it. Answered
 * 0x78, then 0x00 once erased (to 0xFF).
 */
#define ZELOS_UDS_ROUTINE_ERASE 0xFF00
/*
 * checkProgrammingDependencies, programming session and level 0x11. Option
 * record: the expected CRC-32 (IEEE, as zlib) of the last download, 4 bytes
 * big-endian. Result: 0x00 correct or 0x01 incorrect, then the computed CRC-32.
 */
#define ZELOS_UDS_ROUTINE_CHECK 0xFF01

/* A data identifier. Readable in every session. */
struct zelos_uds_did {
	uint16_t id;
	uint8_t size;
	/* Copy the current value, size bytes, into out. */
	void (*read)(uint8_t *out);
	/*
	 * NULL: read-only. Else writable in extended with level 0x01: apply size
	 * bytes, or refuse them with 0x31.
	 */
	uint8_t (*write)(const uint8_t *in);
};

/* InputOutputControlByIdentifier control parameters. */
enum zelos_uds_io_param {
	ZELOS_UDS_IO_RETURN = 0x00,
	ZELOS_UDS_IO_RESET = 0x01,
	ZELOS_UDS_IO_FREEZE = 0x02,
	ZELOS_UDS_IO_ADJUST = 0x03,
};

/* An output a tester may take over (0x2F), in extended with level 0x01. */
struct zelos_uds_io {
	uint16_t id;
	uint8_t size;
	/* The output's current state, size bytes. */
	void (*read)(uint8_t *out);
	/*
	 * Apply a control parameter; `in` is size bytes for ZELOS_UDS_IO_ADJUST,
	 * else NULL. Return 0, or 0x31 for a value out of range. Returning
	 * control must succeed.
	 */
	uint8_t (*control)(uint8_t param, const uint8_t *in);
};

#define ZELOS_UDS_ROUTINE_RESULT_MAX 8

/*
 * A node's routine: started with 0x31 0x01 and no option record in one
 * session, answered 0x78 at once and its result after duration_ms. 0x03
 * repeats the last result; neither 0x03 before a run nor 0x02 is in sequence.
 */
struct zelos_uds_routine {
	uint16_t id;
	uint8_t session;
	uint32_t duration_ms;
	/* Fill the result, at most ZELOS_UDS_ROUTINE_RESULT_MAX bytes; return its length. */
	uint8_t (*run)(uint8_t *result);
};

/*
 * A window ReadMemoryByAddress (0x23) reads, and WriteMemoryByAddress (0x3D,
 * extended with level 0x01) writes if writable. Requests lie wholly inside one.
 */
struct zelos_uds_memory {
	uint32_t address;
	uint32_t size;
	void *data;
	bool writable;
};

/*
 * A J1979 PID (service 0x01) the node supports, as a formula over one of its
 * DIDs. Service 0x02 reports it from snapshot record 0x01 when the DID is in
 * it. PIDs 0x01 and 0x1C, and the bitmaps, are the core's.
 */
struct zelos_uds_pid {
	uint8_t pid;
	uint8_t size;
	uint16_t did;
	/* Encode the DID's value (its size bytes) as the PID's size bytes. */
	void (*encode)(const uint8_t *value, uint8_t *out);
};

#define ZELOS_UDS_DTCS_MAX 16
#define ZELOS_UDS_SNAPSHOT_MAX 32
#define ZELOS_UDS_CALID_LEN 16
#define ZELOS_UDS_ECU_NAME_LEN 20

struct zelos_uds_config {
	const struct zelos_uds_did *dids;
	size_t did_count;
	const struct zelos_uds_io *ios;
	size_t io_count;
	const struct zelos_uds_routine *routines;
	size_t routine_count;
	const struct zelos_uds_memory *memories;
	size_t memory_count;
	/* 3-byte DTC numbers; zelos_uds_dtc() reports each by index. */
	const uint32_t *dtcs;
	size_t dtc_count;
	/* DIDs a DTC's snapshot record 0x01 captures at its first failure. */
	const uint16_t *snapshot_dids;
	size_t snapshot_did_count;
	/*
	 * Every WriteMemoryByAddress's store, e.g. under a lock the application
	 * shares with another writer of the same memory. NULL: stored directly.
	 */
	void (*write)(void *dst, const void *src, size_t len);
	/* OBD-II (CONFIG_ZELOS_UDS_OBD): the PIDs, CALID (<= 16) and ECU name (<= 20). */
	const struct zelos_uds_pid *pids;
	size_t pid_count;
	const char *calid;
	const char *ecu_name;
};

#if defined(CONFIG_ZELOS_UDS)

/**
 * Start answering on the configured identifiers.
 *
 * Call once, after zelos_can_init(). The configuration must outlive the node.
 *
 * @return 0 on success, negative errno otherwise.
 */
int zelos_uds_start(const struct zelos_uds_config *config);

/**
 * Report the result of the test behind DTC index.
 *
 * Call from the application's thread each time the test completes; not at all
 * while it cannot run. Ignored while DTC setting is off (0x85 0x02).
 */
void zelos_uds_dtc(size_t index, bool failed);

/* Whether 0x28 lets the application send, and receive, its normal messages. */
bool zelos_uds_tx_enabled(void);
bool zelos_uds_rx_enabled(void);

#endif /* CONFIG_ZELOS_UDS */

/*
 * The protocol core, free of I/O and Zephyr calls: requests in, responses out.
 * `now` is milliseconds on any monotonic clock. Every response buffer holds
 * CONFIG_ZELOS_UDS_BUF_SIZE bytes, which is also maxNumberOfBlockLength.
 */

enum zelos_uds_session {
	ZELOS_UDS_SESSION_DEFAULT = 0x01,
	ZELOS_UDS_SESSION_PROGRAMMING = 0x02,
	ZELOS_UDS_SESSION_EXTENDED = 0x03,
};

enum zelos_uds_reset {
	ZELOS_UDS_RESET_NONE,
	ZELOS_UDS_RESET_HARD,
	ZELOS_UDS_RESET_SOFT,
};

/* One DTC's memory: Annex D status, counters and snapshot record 0x01. */
struct zelos_uds_dtc_record {
	uint8_t status;
	uint8_t occurrences;
	uint8_t aging;
	/* Permanent (OBD 0x0A), and the passed cycles since towards its release. */
	bool permanent;
	uint8_t passed_cycles;
	bool snapshot_stored;
	uint8_t snapshot[ZELOS_UDS_SNAPSHOT_MAX];
};

/* What a reset keeps. Valid when magic and crc match; else it starts cleared. */
struct zelos_uds_dtc_memory {
	uint32_t magic;
	struct zelos_uds_dtc_record dtc[ZELOS_UDS_DTCS_MAX];
	uint32_t crc;
};

enum zelos_uds_transfer {
	ZELOS_UDS_TRANSFER_NONE,
	ZELOS_UDS_TRANSFER_DOWNLOAD,
	ZELOS_UDS_TRANSFER_UPLOAD,
};

struct zelos_uds_core {
	const struct zelos_uds_config *config;
	/* A random 32-bit value; the core retries a zero. */
	uint32_t (*random)(void);
	struct zelos_uds_dtc_memory *memory;

	uint8_t session;
	int64_t s3_deadline;

	/* The level unlocked, 0: none. */
	uint8_t unlocked;
	/* The seed awaiting its key, and its level; 0: none. */
	uint32_t seed;
	uint8_t seed_level;
	/* Per level: extended, programming. */
	uint8_t failed_keys[2];
	int64_t lockout_until[2];

	/* 0x28 control type: 0x00, 0x01 or 0x03. */
	uint8_t communication;
	bool dtc_setting;

	/* Bit i: the tester controls ios[i]. */
	uint32_t io_active;

	/* A routine answered 0x78 and not yet finished; its result is pending. */
	uint16_t pending;
	const struct zelos_uds_routine *pending_routine;
	int64_t pending_done;
	int64_t pending_sent;
	/* The last finished routine, for requestResults. */
	uint16_t last;
	uint8_t last_result[ZELOS_UDS_ROUTINE_RESULT_MAX];
	uint8_t last_len;

	/* The download window: erased range, last download, transfer in progress. */
	uint32_t erase_addr;
	uint32_t erase_size;
	uint32_t erased_lo;
	uint32_t erased_hi;
	bool downloaded;
	uint32_t downloaded_addr;
	uint32_t downloaded_size;
	uint8_t transfer;
	uint32_t transfer_addr;
	uint32_t transfer_size;
	uint32_t transfer_done;
	uint8_t transfer_bsc;
	uint32_t transfer_last_len;
	uint8_t download[CONFIG_ZELOS_UDS_DOWNLOAD_SIZE];

	/* Set by ECUReset: reset once its response, if any, is sent. */
	uint8_t reset;
};

/**
 * Start a power-on: memory kept from before a reset begins a new operation
 * cycle; anything else starts cleared.
 *
 * @return 0, or -EINVAL if the configuration exceeds the core's limits.
 */
int zelos_uds_core_init(struct zelos_uds_core *uds, const struct zelos_uds_config *config,
			uint32_t (*random)(void), struct zelos_uds_dtc_memory *memory, int64_t now);

/**
 * Handle one request.
 *
 * @param functional The request arrived on the functional identifier.
 * @return Length of the response written to rsp; 0: send nothing.
 */
size_t zelos_uds_core_request(struct zelos_uds_core *uds, const uint8_t *req, size_t len,
			      bool functional, int64_t now, uint8_t *rsp);

/**
 * Advance the timers: S3, and a pending routine's 0x78 repeats and result.
 *
 * Call at least every 10 ms. @return Length of a response to send, or 0.
 */
size_t zelos_uds_core_tick(struct zelos_uds_core *uds, int64_t now, uint8_t *rsp);

void zelos_uds_core_dtc(struct zelos_uds_core *uds, size_t index, bool failed);

/* Seal the DTC memory for the next boot. Call right before an orderly reset. */
void zelos_uds_core_persist(struct zelos_uds_core *uds);

#endif /* ZELOS_UDS_H_ */
