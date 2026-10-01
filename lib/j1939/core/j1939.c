/*
 * J1939 core: no Zephyr calls, no I/O, no sleeping. See include/zelos/j1939.h.
 */

#include <zelos/j1939.h>

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define PF(pgn)  (((pgn) >> 8) & 0xFFU)
#define PDU1(pgn) (PF(pgn) < 240U)

#define PRIO_CLAIM 6U
#define PRIO_TP    7U

#define TP_CM_BAM 32U
#define ACK_NACK  1U

/* Where an arbitrary-address-capable node looks when it loses its address. */
#define DYNAMIC_FIRST 128U
#define DYNAMIC_LAST  247U
#define DYNAMIC(a)    ((a) >= DYNAMIC_FIRST && (a) <= DYNAMIC_LAST)

/* Wrap-safe: has `now` reached `t`? */
static bool reached(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

static uint32_t make_id(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa)
{
	if (PDU1(pgn)) {
		pgn = (pgn & 0x3FF00U) | da;
	}
	return ((uint32_t)(prio & 7U) << 26) | ((pgn & 0x3FFFFU) << 8) | sa;
}

static void emit(struct zelos_j1939 *j, uint8_t prio, uint32_t pgn, const uint8_t *data,
		 uint8_t len)
{
	struct zelos_j1939_frame f = {
		.id = make_id(prio, pgn, ZELOS_J1939_ADDR_GLOBAL, j->address),
		.len = len,
	};

	memcpy(f.data, data, len);
	j->send(&f, j->user);
}

/*
 * J1939-81 has several nodes that cannot claim answer a request after a
 * pseudo-random 0 to 153 ms, so their identical IDs do not collide. The NAME
 * is unique, so it seeds the delay.
 */
static uint32_t cannot_claim_delay(const struct zelos_j1939 *j)
{
	return (uint32_t)((j->name ^ (j->name >> 32)) % 154U);
}

static void claim(struct zelos_j1939 *j, enum zelos_j1939_state state, uint8_t address,
		  uint32_t now_ms)
{
	j->state = state;
	j->address = address;
	j->claim_pending = true;
	j->claim_started = false;
	j->claim_due_ms = now_ms;
	j->bam.active = false;
}

static void lose(struct zelos_j1939 *j, uint32_t now_ms)
{
	bool arbitrary = (j->name >> 63) != 0U;

	/* Each dynamic address at most once, the preferred one included. */
	if (arbitrary && j->tried < DYNAMIC_LAST - DYNAMIC_FIRST + 1U) {
		uint8_t next = j->address + 1U;

		if (!DYNAMIC(next)) {
			next = DYNAMIC_FIRST;
		}
		j->tried++;
		claim(j, ZELOS_J1939_CLAIMING, next, now_ms);
		return;
	}

	claim(j, ZELOS_J1939_CANNOT_CLAIM, ZELOS_J1939_ADDR_NULL, now_ms);
}

int zelos_j1939_init(struct zelos_j1939 *j, uint32_t now_ms)
{
	for (size_t i = 0; i < j->n_msgs; i++) {
		/* Sent to global only: a PDU1 PGN would need the requester as DA. */
		if (PDU1(j->msgs[i].pgn)) {
			return -EINVAL;
		}
		j->msgs[i].requested = false;
	}
	j->tried = DYNAMIC(j->preferred) ? 1U : 0U;
	j->nack_pending = false;
	claim(j, ZELOS_J1939_CLAIMING, j->preferred, now_ms);
	return 0;
}

static void on_claim(struct zelos_j1939 *j, uint8_t sa, const struct zelos_j1939_frame *f,
		     uint32_t now_ms)
{
	uint64_t theirs;

	if (f->len < 8U || j->state == ZELOS_J1939_CANNOT_CLAIM || sa != j->address) {
		return;
	}

	theirs = sys_get_le64(f->data);
	if (theirs == j->name) {
		return;
	}
	if (j->name < theirs) {
		/* We keep it: say so again, and the contender moves. */
		j->claim_pending = true;
		j->claim_due_ms = now_ms;
		return;
	}
	lose(j, now_ms);
}

static void on_request(struct zelos_j1939 *j, uint8_t da, uint8_t sa,
		       const struct zelos_j1939_frame *f, uint32_t now_ms)
{
	uint32_t pgn;

	if (f->len < 3U || (da != ZELOS_J1939_ADDR_GLOBAL && da != j->address)) {
		return;
	}

	pgn = sys_get_le24(f->data);

	if (pgn == ZELOS_J1939_PGN_ADDRESS_CLAIMED) {
		j->claim_pending = true;
		j->claim_due_ms = now_ms + (j->state == ZELOS_J1939_CANNOT_CLAIM
						    ? cannot_claim_delay(j)
						    : 0U);
		return;
	}
	if (j->state != ZELOS_J1939_CLAIMED) {
		return;
	}

	for (size_t i = 0; i < j->n_msgs; i++) {
		if (j->msgs[i].pgn == pgn) {
			j->msgs[i].requested = true;
			return;
		}
	}

	/* J1939-21: a request addressed to us for a PGN we lack gets a NACK. */
	if (da != ZELOS_J1939_ADDR_GLOBAL) {
		j->nack_pending = true;
		j->nack_to = sa;
		j->nack_pgn = pgn;
	}
}

void zelos_j1939_on_frame(struct zelos_j1939 *j, const struct zelos_j1939_frame *frame,
			  uint32_t now_ms)
{
	uint32_t pgn = (frame->id >> 8) & 0x3FFFFU;
	uint8_t sa = frame->id & 0xFFU;
	uint8_t da = ZELOS_J1939_ADDR_GLOBAL;

	if (PDU1(pgn)) {
		da = pgn & 0xFFU;
		pgn &= 0x3FF00U;
	}

	if (pgn == ZELOS_J1939_PGN_ADDRESS_CLAIMED) {
		on_claim(j, sa, frame, now_ms);
	} else if (pgn == ZELOS_J1939_PGN_REQUEST) {
		on_request(j, da, sa, frame, now_ms);
	}
}

static void send_claim(struct zelos_j1939 *j, uint32_t now_ms)
{
	uint8_t name[8];

	sys_put_le64(j->name, name);
	emit(j, PRIO_CLAIM, ZELOS_J1939_PGN_ADDRESS_CLAIMED, name, sizeof(name));
	j->claim_pending = false;

	if (j->state == ZELOS_J1939_CLAIMING && !j->claim_started) {
		j->claim_started = true;
		j->claimed_ms = now_ms;
	}
}

static void send_nack(struct zelos_j1939 *j)
{
	uint8_t d[8] = {ACK_NACK, 0xFF, 0xFF, 0xFF, j->nack_to};

	sys_put_le24(j->nack_pgn, &d[5]);
	emit(j, PRIO_CLAIM, ZELOS_J1939_PGN_ACK, d, sizeof(d));
	j->nack_pending = false;
}

static void bam_start(struct zelos_j1939 *j, const struct zelos_j1939_msg *m, uint32_t now_ms)
{
	uint8_t cm[8] = {TP_CM_BAM};

	j->bam.active = true;
	j->bam.pgn = m->pgn;
	j->bam.len = m->len;
	j->bam.seq = 1U;
	j->bam.packets = (uint8_t)((m->len + 6U) / 7U);
	j->bam.due_ms = now_ms + ZELOS_J1939_BAM_GAP_MS;
	memcpy(j->bam.buf, m->data, m->len);

	sys_put_le16(m->len, &cm[1]);
	cm[3] = j->bam.packets;
	cm[4] = 0xFF;
	sys_put_le24(m->pgn, &cm[5]);
	emit(j, PRIO_TP, ZELOS_J1939_PGN_TP_CM, cm, sizeof(cm));
}

/* One packet per call at most, so the gap is never shorter than BAM_GAP_MS. */
static void bam_step(struct zelos_j1939 *j, uint32_t now_ms)
{
	uint8_t dt[8];
	size_t off;
	size_t n;

	if (!j->bam.active || !reached(now_ms, j->bam.due_ms)) {
		return;
	}

	off = (size_t)(j->bam.seq - 1U) * 7U;
	n = j->bam.len - off < 7U ? j->bam.len - off : 7U;
	memset(dt, 0xFF, sizeof(dt));
	dt[0] = j->bam.seq;
	memcpy(&dt[1], &j->bam.buf[off], n);
	emit(j, PRIO_TP, ZELOS_J1939_PGN_TP_DT, dt, sizeof(dt));

	j->bam.due_ms = now_ms + ZELOS_J1939_BAM_GAP_MS;
	j->bam.active = j->bam.seq++ < j->bam.packets;
}

static void send_msgs(struct zelos_j1939 *j, uint32_t now_ms)
{
	for (size_t i = 0; i < j->n_msgs; i++) {
		struct zelos_j1939_msg *m = &j->msgs[i];
		bool periodic = m->period_ms != 0U && reached(now_ms, m->due_ms);

		if (!periodic && !m->requested) {
			continue;
		}

		if (m->len > 8U) {
			/* One BAM at a time per sender; this one waits its turn. */
			if (j->bam.active || m->len > ZELOS_J1939_MAX_LEN) {
				continue;
			}
			bam_start(j, m, now_ms);
		} else {
			emit(j, m->priority, m->pgn, m->data, (uint8_t)m->len);
		}

		m->requested = false;
		if (periodic) {
			/* Keep the phase, but after a stall resume rather than burst. */
			m->due_ms += m->period_ms;
			if (reached(now_ms, m->due_ms)) {
				m->due_ms = now_ms + m->period_ms;
			}
		}
	}
}

void zelos_j1939_poll(struct zelos_j1939 *j, uint32_t now_ms)
{
	if (j->claim_pending && reached(now_ms, j->claim_due_ms)) {
		send_claim(j, now_ms);
	}

	if (j->state == ZELOS_J1939_CLAIMING && j->claim_started &&
	    reached(now_ms, j->claimed_ms + ZELOS_J1939_CLAIM_WAIT_MS)) {
		j->state = ZELOS_J1939_CLAIMED;
		/* Periods run from here, so a move neither bunches nor shifts them. */
		for (size_t i = 0; i < j->n_msgs; i++) {
			j->msgs[i].due_ms = now_ms;
		}
	}
	if (j->state != ZELOS_J1939_CLAIMED) {
		return;
	}

	if (j->nack_pending) {
		send_nack(j);
	}
	bam_step(j, now_ms);
	send_msgs(j, now_ms);
}
