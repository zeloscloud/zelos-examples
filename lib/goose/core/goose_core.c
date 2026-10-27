/*
 * GOOSE publisher, no I/O: the retransmission schedule and the frame, an
 * Ethernet II header, the session header (APPID, Length, two reserved words)
 * and a BER IECGoosePdu (IEC 61850-8-1 Annex A), definite lengths, allData as
 * MMS Data (boolean [3], bit-string [4], integer [5], unsigned [6],
 * floating-point [7]).
 */

#include <zelos/goose.h>

#include <string.h>

/* TimeQuality: clock not synchronized, accuracy unspecified. */
#define TIME_QUALITY 0x3F
#define PDU_TAG 0x61
#define BURST_FIRST_MS 4
#define USEC_PER_S 1000000
#define USEC_PER_MS 1000

#define PRIMITIVE(n) (0x80 | (n))
#define CONSTRUCTED(n) (0xA0 | (n))

struct cursor {
	uint8_t *p;
	uint8_t *end;
};

uint32_t zelos_goose_next(uint32_t n)
{
	return n == UINT32_MAX ? 1U : n + 1U;
}

uint32_t zelos_goose_interval_ms(uint8_t step)
{
	return step < ZELOS_GOOSE_BURST ? BURST_FIRST_MS << step : ZELOS_GOOSE_STEADY_MS;
}

static bool put(struct cursor *c, const void *src, size_t n)
{
	if ((size_t)(c->end - c->p) < n) {
		return false;
	}
	memcpy(c->p, src, n);
	c->p += n;
	return true;
}

static bool put_u8(struct cursor *c, uint8_t v)
{
	return put(c, &v, 1);
}

static size_t len_size(size_t len)
{
	return len < 0x80 ? 1 : len <= 0xFF ? 2 : 3;
}

/* Definite length, short or long form. */
static bool put_len(struct cursor *c, size_t len)
{
	if (len < 0x80) {
		return put_u8(c, (uint8_t)len);
	}
	if (len <= 0xFF) {
		return put_u8(c, 0x81) && put_u8(c, (uint8_t)len);
	}
	return put_u8(c, 0x82) && put_u8(c, (uint8_t)(len >> 8)) && put_u8(c, (uint8_t)len);
}

static bool put_tlv(struct cursor *c, uint8_t tag, const void *v, size_t len)
{
	return put_u8(c, tag) && put_len(c, len) && put(c, v, len);
}

static bool put_str(struct cursor *c, uint8_t tag, const char *s)
{
	return put_tlv(c, tag, s, strlen(s));
}

/* INTEGER, minimal two's complement: a leading byte that only repeats the sign goes. */
static bool put_int(struct cursor *c, uint8_t tag, int64_t v)
{
	uint8_t be[8];
	size_t i = 0;

	for (size_t k = 0; k < sizeof(be); k++) {
		be[k] = (uint8_t)((uint64_t)v >> (56 - 8 * k));
	}
	while (i < sizeof(be) - 1 && ((be[i] == 0x00 && (be[i + 1] & 0x80) == 0) ||
				      (be[i] == 0xFF && (be[i + 1] & 0x80) != 0))) {
		i++;
	}
	return put_tlv(c, tag, be + i, sizeof(be) - i);
}

static bool put_bool(struct cursor *c, uint8_t tag, bool v)
{
	return put_tlv(c, tag, &(uint8_t){v ? 0x01 : 0x00}, 1);
}

static bool put_value(struct cursor *c, const struct zelos_goose_value *v)
{
	uint32_t bits;

	switch (v->type) {
	case ZELOS_GOOSE_BOOLEAN:
		return put_bool(c, PRIMITIVE(3), v->boolean);
	case ZELOS_GOOSE_DBPOS:
		/* 2 bits, MSB first; 6 of the octet unused. */
		return put_tlv(c, PRIMITIVE(4), (uint8_t[]){6, (uint8_t)((v->dbpos & 0x3) << 6)},
			       2);
	case ZELOS_GOOSE_INT32:
		return put_int(c, PRIMITIVE(5), v->i32);
	case ZELOS_GOOSE_UINT32:
		return put_int(c, PRIMITIVE(6), v->u32);
	case ZELOS_GOOSE_FLOAT32:
		/* Exponent width 8, then IEEE 754 single, big-endian. */
		memcpy(&bits, &v->f32, sizeof(bits));
		return put_tlv(c, PRIMITIVE(7),
			       (uint8_t[]){8, bits >> 24, bits >> 16, bits >> 8, bits}, 5);
	default:
		return false;
	}
}

static bool put_body(struct cursor *c, const struct zelos_goose_pub *pub, uint32_t tal_ms)
{
	const struct zelos_goose_cb *cb = pub->cb;
	const uint32_t s = (uint32_t)(pub->t_us / USEC_PER_S);
	/* The fraction of a second in 24 bits. */
	const uint32_t frac = (uint32_t)(((uint64_t)(pub->t_us % USEC_PER_S) << 24) / USEC_PER_S);
	const uint8_t t[8] = {s >> 24,   s >> 16,  s >> 8, s,
			      frac >> 16, frac >> 8, frac,   TIME_QUALITY};
	uint8_t data[ZELOS_GOOSE_FRAME_MAX];
	struct cursor d = {data, data + sizeof(data)};

	if (cb->count > ZELOS_GOOSE_MEMBERS_MAX) {
		return false;
	}
	for (size_t i = 0; i < cb->count; i++) {
		if (!put_value(&d, &pub->values[i])) {
			return false;
		}
	}

	return put_str(c, PRIMITIVE(0), cb->gocb_ref) && put_int(c, PRIMITIVE(1), tal_ms) &&
	       put_str(c, PRIMITIVE(2), cb->dat_set) && put_str(c, PRIMITIVE(3), cb->go_id) &&
	       put_tlv(c, PRIMITIVE(4), t, sizeof(t)) && put_int(c, PRIMITIVE(5), pub->st_num) &&
	       put_int(c, PRIMITIVE(6), pub->sq_num) &&
	       put_bool(c, PRIMITIVE(7), pub->simulation) &&
	       put_int(c, PRIMITIVE(8), pub->conf_rev) && put_bool(c, PRIMITIVE(9), pub->nds_com) &&
	       put_int(c, PRIMITIVE(10), cb->count) &&
	       put_tlv(c, CONSTRUCTED(11), data, d.p - data);
}

static size_t frame(uint8_t *out, size_t cap, const uint8_t src_mac[6],
		    const struct zelos_goose_pub *pub, uint32_t tal_ms)
{
	const struct zelos_goose_cb *cb = pub->cb;
	uint8_t body[ZELOS_GOOSE_FRAME_MAX];
	struct cursor b = {body, body + sizeof(body)};
	struct cursor c = {out, out + cap};
	size_t body_len;
	size_t length;

	if (!put_body(&b, pub, tal_ms)) {
		return 0;
	}
	body_len = b.p - body;
	/* APPID to the PDU's end. */
	length = 8 + 1 + len_size(body_len) + body_len;

	const uint8_t hdr[10] = {ZELOS_GOOSE_ETHERTYPE >> 8, ZELOS_GOOSE_ETHERTYPE & 0xFF,
				 cb->appid >> 8, cb->appid & 0xFF, length >> 8, length & 0xFF,
				 0, 0, 0, 0};

	if (!put(&c, cb->dst, 6) || !put(&c, src_mac, 6) || !put(&c, hdr, sizeof(hdr)) ||
	    !put_u8(&c, PDU_TAG) || !put_len(&c, body_len) || !put(&c, body, body_len)) {
		return 0;
	}
	return c.p - out;
}

void zelos_goose_update(struct zelos_goose_pub *pub, uint32_t state, uint32_t conf_rev,
			int64_t now)
{
	const bool first = pub->st_num == 0;

	if (!first && state == pub->state && conf_rev == pub->conf_rev) {
		return;
	}
	pub->st_num = zelos_goose_next(pub->st_num);
	pub->sq_num = 0;
	pub->state = state;
	pub->conf_rev = conf_rev;
	pub->t_us = now;
	pub->step = 0;
	/* On change, at once; a periodic one keeps its period. */
	if (first || pub->cb->period_ms == 0) {
		pub->next_us = now;
	}
}

size_t zelos_goose_poll(struct zelos_goose_pub *pub, int64_t now, const uint8_t src_mac[6],
			uint8_t *out, size_t cap)
{
	uint32_t interval;
	size_t len;

	if (pub->st_num == 0 || now < pub->next_us) {
		return 0;
	}
	interval = pub->cb->period_ms != 0 ? pub->cb->period_ms
					   : zelos_goose_interval_ms(pub->step);
	len = frame(out, cap, src_mac, pub, 2 * interval);

	pub->sq_num = zelos_goose_next(pub->sq_num);
	if (pub->step < ZELOS_GOOSE_BURST) {
		pub->step++;
	}
	pub->next_us += (int64_t)interval * USEC_PER_MS;
	/* Late: on from now, no catch-up burst. */
	if (pub->next_us <= now) {
		pub->next_us = now + (int64_t)interval * USEC_PER_MS;
	}
	return len;
}
