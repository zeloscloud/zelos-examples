/*
 * IEC 61850-8-1 GOOSE publisher, Edition 2 (tag [7] is simulation): control
 * blocks on raw Ethernet, EtherType 0x88B8, no VLAN tag. Nothing is received.
 *
 * A control block publishes either on change (period_ms 0) or every
 * period_ms, read when its frame is due. stNum counts changes of the state its
 * read() returns, and of confRev: a change sets sqNum to 0 and t to now. On
 * change, the change's frame goes at once, then repeats 4, 8, 16, 32, 64, 128,
 * 256 and 512 ms apart, then every 1000 ms. timeAllowedtoLive is twice the
 * time to the next frame: 8 ms on the change's, 2000 ms once steady;
 * 2 x period_ms otherwise.
 * stNum and sqNum wrap to 1, not 0 (IEC 61850-7-2). t is time since boot from
 * 1970-01-01, its quality "clock not synchronized".
 *
 * Test knobs, every control block at once: UDS RoutineControl
 * ZELOS_GOOSE_ROUTINE (extended session; with lib/uds), option record one
 * byte: 0x01 toggles simulation, 0x02 toggles ndsCom, 0x03 adds one to confRev
 * (a new state). Any other changes nothing. Result: simulation, ndsCom (0/1),
 * confRev (uint32 big-endian).
 */

#ifndef ZELOS_GOOSE_H_
#define ZELOS_GOOSE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZELOS_GOOSE_ETHERTYPE 0x88B8
#define ZELOS_GOOSE_MEMBERS_MAX 24
/* Largest frame zelos_goose_poll() writes. */
#define ZELOS_GOOSE_FRAME_MAX 320
/* Repeats after a change before the steady interval. */
#define ZELOS_GOOSE_BURST 8
#define ZELOS_GOOSE_STEADY_MS 1000
/* A RoutineControl identifier of the vehicle manufacturer's range. */
#define ZELOS_GOOSE_ROUTINE 0x6185

enum zelos_goose_type {
	ZELOS_GOOSE_BOOLEAN,
	/* Dbpos (IEC 61850-7-3): a 2-bit BIT STRING, enum zelos_goose_dbpos. */
	ZELOS_GOOSE_DBPOS,
	ZELOS_GOOSE_INT32,
	ZELOS_GOOSE_UINT32,
	ZELOS_GOOSE_FLOAT32,
};

enum zelos_goose_dbpos {
	ZELOS_GOOSE_INTERMEDIATE = 0,
	ZELOS_GOOSE_OFF = 1,
	ZELOS_GOOSE_ON = 2,
	ZELOS_GOOSE_BAD = 3,
};

struct zelos_goose_value {
	uint8_t type;
	union {
		bool boolean;
		uint8_t dbpos;
		int32_t i32;
		uint32_t u32;
		float f32;
	};
};

struct zelos_goose_cb {
	uint16_t appid;
	uint8_t dst[6];
	const char *gocb_ref;
	const char *dat_set;
	const char *go_id;
	/* 0: on change. */
	uint16_t period_ms;
	/* Data set members, at most ZELOS_GOOSE_MEMBERS_MAX. */
	uint8_t count;
	/* Fill count values; return the state, whose changes stNum counts. */
	uint32_t (*read)(struct zelos_goose_value *values);
};

#if defined(CONFIG_ZELOS_GOOSE)

/**
 * Publish the control blocks once lib/eth's link is up. Call once; they must
 * outlive the node. 0, or -errno: -EINVAL if a block's frame could exceed
 * ZELOS_GOOSE_FRAME_MAX, found by encoding one on the caller's stack (about
 * 1 KB).
 */
int zelos_goose_start(const struct zelos_goose_cb *cbs, size_t count);

/* A member may have changed: read every on-change control block now. Any context. */
void zelos_goose_poke(void);

/* The UDS routine ZELOS_GOOSE_ROUTINE: option[0] as above; returns the result's length. */
uint8_t zelos_goose_routine(const uint8_t *option, uint8_t *result);

#endif /* CONFIG_ZELOS_GOOSE */

/*
 * The publisher, free of I/O. `now` in us, so a change's first repeat is 4 ms
 * after it, not after the millisecond it fell in. One thread drives it: write
 * values (cb->read), zelos_goose_update(), then zelos_goose_poll() until 0.
 */

struct zelos_goose_pub {
	const struct zelos_goose_cb *cb;
	/* 0 until the first update. */
	uint32_t st_num;
	uint32_t sq_num;
	uint32_t conf_rev;
	bool simulation;
	bool nds_com;
	uint32_t state;
	/* The last change, and when the next frame is due, us. */
	int64_t t_us;
	int64_t next_us;
	/* Frames since the change, up to ZELOS_GOOSE_BURST. */
	uint8_t step;
	struct zelos_goose_value values[ZELOS_GOOSE_MEMBERS_MAX];
};

/* The counters' successor: 2^32 - 1 wraps to 1. */
uint32_t zelos_goose_next(uint32_t n);

/* On change: ms from frame `step` (0, the change's) to the next. */
uint32_t zelos_goose_interval_ms(uint8_t step);

/* A new state or confRev starts stNum + 1; the first update starts stNum 1. */
void zelos_goose_update(struct zelos_goose_pub *pub, uint32_t state, uint32_t conf_rev,
			int64_t now);

/*
 * The frame due by now, Ethernet header included, into out; its length, or 0
 * when none is due (or cap is too small). Advances sqNum and the schedule.
 */
size_t zelos_goose_poll(struct zelos_goose_pub *pub, int64_t now, const uint8_t src_mac[6],
			uint8_t *out, size_t cap);

#endif /* ZELOS_GOOSE_H_ */
