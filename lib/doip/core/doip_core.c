/* DoIP entity core (ISO 13400-2:2012). No I/O: see include/zelos/doip.h. */

#include <zelos/doip.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define VERSION 0x02
#define VERSION_2019 0x03
#define VERSION_ANY 0xFF

enum {
	GENERIC_NACK = 0x0000,
	IDENTIFY = 0x0001,
	IDENTIFY_EID = 0x0002,
	IDENTIFY_VIN = 0x0003,
	ANNOUNCEMENT = 0x0004,
	ACTIVATE = 0x0005,
	ACTIVATION = 0x0006,
	ALIVE_CHECK = 0x0007,
	ALIVE_CHECK_RESPONSE = 0x0008,
	ENTITY_STATUS = 0x4001,
	ENTITY_STATUS_RESPONSE = 0x4002,
	POWER_MODE = 0x4003,
	POWER_MODE_RESPONSE = 0x4004,
	DIAGNOSTIC = 0x8001,
	DIAGNOSTIC_ACK = 0x8002,
	DIAGNOSTIC_NACK = 0x8003,
};

/* Generic header NACK codes. */
enum {
	INCORRECT_PATTERN = 0x00,
	UNKNOWN_TYPE = 0x01,
	TOO_LARGE = 0x02,
	INVALID_LENGTH = 0x04,
};

/* Routing activation response codes. */
enum {
	UNKNOWN_SOURCE = 0x00,
	ALL_IN_USE = 0x01,
	SOURCE_DIFFERS = 0x02,
	SOURCE_ACTIVE = 0x03,
	UNSUPPORTED_TYPE = 0x06,
	ACTIVATED = 0x10,
};

/* Diagnostic message NACK codes. */
enum {
	INVALID_SOURCE = 0x02,
	UNKNOWN_TARGET = 0x03,
	MESSAGE_TOO_LARGE = 0x04,
};

#define TESTER_MIN 0x0E00
#define TESTER_MAX 0x0FFF
#define NODE 0x01
#define POWER_READY 0x01

/* T_TCP_Initial_Inactivity, T_TCP_General_Inactivity, T_TCP_Alive_Check. */
#define INITIAL_INACTIVITY_MS 2000
#define GENERAL_INACTIVITY_MS 300000
#define ALIVE_CHECK_MS 500

#define FOREVER INT64_MAX

static size_t header(uint8_t *out, uint16_t type, size_t len)
{
	out[0] = VERSION;
	out[1] = (uint8_t)~VERSION;
	sys_put_be16(type, &out[2]);
	sys_put_be32((uint32_t)len, &out[4]);
	return ZELOS_DOIP_HEADER_SIZE + len;
}

static size_t nack(uint8_t *out, uint8_t code)
{
	out[ZELOS_DOIP_HEADER_SIZE] = code;
	return header(out, GENERIC_NACK, 1);
}

static bool version_ok(const uint8_t *h, bool any)
{
	return (h[0] ^ h[1]) == 0xFF &&
	       (h[0] == VERSION || h[0] == VERSION_2019 || (any && h[0] == VERSION_ANY));
}

void zelos_doip_core_init(struct zelos_doip_core *doip, const struct zelos_doip_config *config,
			  const uint8_t eid[6], const struct zelos_doip_ops *ops, void *ctx)
{
	memset(doip, 0, sizeof(*doip));
	doip->config = config;
	doip->ops = ops;
	doip->ctx = ctx;
	memcpy(doip->eid, eid, sizeof(doip->eid));
}

uint8_t zelos_doip_core_testers(const struct zelos_doip_core *doip)
{
	uint8_t n = 0;

	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		n += doip->socket[i].state == ZELOS_DOIP_ACTIVE;
	}
	return n;
}

int zelos_doip_core_accept(struct zelos_doip_core *doip, int64_t now)
{
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		struct zelos_doip_socket *s = &doip->socket[i];

		if (s->state == ZELOS_DOIP_FREE) {
			memset(s, 0, sizeof(*s));
			s->state = ZELOS_DOIP_OPEN;
			s->idle_until = now + INITIAL_INACTIVITY_MS;
			return i;
		}
	}
	return -1;
}

void zelos_doip_core_close(struct zelos_doip_core *doip, int s)
{
	if (doip->socket[s].state == ZELOS_DOIP_FREE) {
		return;
	}
	doip->ops->close(doip->ctx, s);
	doip->socket[s].state = ZELOS_DOIP_FREE;
}

static void send(struct zelos_doip_core *doip, int s, size_t len)
{
	if (!doip->ops->send(doip->ctx, s, doip->out, len)) {
		zelos_doip_core_close(doip, s);
	}
}

static void send_nack(struct zelos_doip_core *doip, int s, uint8_t code)
{
	send(doip, s, nack(doip->out, code));
}

static void alive_check(struct zelos_doip_core *doip, int s, int64_t now)
{
	struct zelos_doip_socket *sock = &doip->socket[s];

	if (sock->alive_wait || sock->alive) {
		return;
	}
	sock->alive_wait = true;
	sock->alive_until = now + ALIVE_CHECK_MS;
	send(doip, s, header(doip->out, ALIVE_CHECK, 0));
}

static void answer_activation(struct zelos_doip_core *doip, int s, uint8_t code)
{
	struct zelos_doip_socket *sock = &doip->socket[s];
	uint8_t *p = &doip->out[ZELOS_DOIP_HEADER_SIZE];

	sys_put_be16(sock->activate_source, &p[0]);
	sys_put_be16(doip->config->logical_address, &p[2]);
	p[4] = code;
	/* Reserved by ISO. */
	sys_put_be32(0, &p[5]);
	sock->activating = false;
	send(doip, s, header(doip->out, ACTIVATION, 9));
	if (code != ACTIVATED) {
		zelos_doip_core_close(doip, s);
	}
	/* A round's alive answers count for that round only. */
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		doip->socket[i].alive = false;
	}
}

/* Decide socket s's routing activation, or start the alive checks it waits on. */
static void activate(struct zelos_doip_core *doip, int s, int64_t now)
{
	struct zelos_doip_socket *sock = &doip->socket[s];
	const uint16_t source = sock->activate_source;
	bool waiting = false;
	int others = 0;
	int alive = 0;

	if (source < TESTER_MIN || source > TESTER_MAX) {
		answer_activation(doip, s, UNKNOWN_SOURCE);
		return;
	}
	if (sock->activate_type != 0x00 && sock->activate_type != 0xE0) {
		answer_activation(doip, s, UNSUPPORTED_TYPE);
		return;
	}
	if (sock->state == ZELOS_DOIP_ACTIVE) {
		answer_activation(doip, s, sock->tester == source ? ACTIVATED : SOURCE_DIFFERS);
		return;
	}
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		struct zelos_doip_socket *o = &doip->socket[i];

		if (i == s || o->state != ZELOS_DOIP_ACTIVE) {
			continue;
		}
		if (o->tester == source) {
			if (o->alive) {
				answer_activation(doip, s, SOURCE_ACTIVE);
				return;
			}
			alive_check(doip, i, now);
			waiting = true;
		}
		others++;
		alive += o->alive;
	}
	if (!waiting && others >= ZELOS_DOIP_TESTERS) {
		if (alive == others) {
			answer_activation(doip, s, ALL_IN_USE);
			return;
		}
		for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
			if (i != s && doip->socket[i].state == ZELOS_DOIP_ACTIVE) {
				alive_check(doip, i, now);
			}
		}
		waiting = true;
	}
	if (waiting) {
		sock->activating = true;
		return;
	}
	sock->state = ZELOS_DOIP_ACTIVE;
	sock->tester = source;
	sock->idle_until = now + GENERAL_INACTIVITY_MS;
	answer_activation(doip, s, ACTIVATED);
}

/* Resume activations once no alive check they wait on is outstanding. */
static void resume(struct zelos_doip_core *doip, int64_t now)
{
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		if (doip->socket[i].alive_wait) {
			return;
		}
	}
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		if (doip->socket[i].state != ZELOS_DOIP_FREE && doip->socket[i].activating) {
			activate(doip, i, now);
		}
	}
}

static void diagnostic_nack(struct zelos_doip_core *doip, int s, uint16_t source,
			    uint16_t target, uint8_t code)
{
	uint8_t *p = &doip->out[ZELOS_DOIP_HEADER_SIZE];

	sys_put_be16(target, &p[0]);
	sys_put_be16(source, &p[2]);
	p[4] = code;
	send(doip, s, header(doip->out, DIAGNOSTIC_NACK, 5));
}

static void diagnostic(struct zelos_doip_core *doip, int s, const uint8_t *p, size_t len)
{
	struct zelos_doip_socket *sock = &doip->socket[s];
	const uint16_t source = sys_get_be16(&p[0]);
	const uint16_t target = sys_get_be16(&p[2]);
	const bool functional = target == doip->config->functional_address;
	uint8_t *out = &doip->out[ZELOS_DOIP_HEADER_SIZE];
	size_t n;

	if (sock->state != ZELOS_DOIP_ACTIVE || sock->tester != source) {
		diagnostic_nack(doip, s, source, target, INVALID_SOURCE);
		zelos_doip_core_close(doip, s);
		return;
	}
	if (target != doip->config->logical_address && !functional) {
		diagnostic_nack(doip, s, source, target, UNKNOWN_TARGET);
		return;
	}
	sys_put_be16(target, &out[0]);
	sys_put_be16(source, &out[2]);
	out[4] = 0x00;
	send(doip, s, header(doip->out, DIAGNOSTIC_ACK, 5));
	if (sock->state != ZELOS_DOIP_ACTIVE) {
		return;
	}
	n = doip->ops->diagnose(doip->ctx, source, &p[4], len - 4, functional, &out[4]);
	if (n > 0 && sock->state == ZELOS_DOIP_ACTIVE) {
		sys_put_be16(doip->config->logical_address, &out[0]);
		sys_put_be16(source, &out[2]);
		send(doip, s, header(doip->out, DIAGNOSTIC, 4 + n));
	}
}

void zelos_doip_core_respond(struct zelos_doip_core *doip, uint16_t tester, const uint8_t *rsp,
			     size_t len)
{
	uint8_t *out = &doip->out[ZELOS_DOIP_HEADER_SIZE];

	if (len > ZELOS_DOIP_MDS - 4) {
		return;
	}
	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		if (doip->socket[i].state == ZELOS_DOIP_ACTIVE && doip->socket[i].tester == tester) {
			sys_put_be16(doip->config->logical_address, &out[0]);
			sys_put_be16(tester, &out[2]);
			memcpy(&out[4], rsp, len);
			send(doip, i, header(doip->out, DIAGNOSTIC, 4 + len));
			return;
		}
	}
}

/* One whole message in sock->buf. */
static void message(struct zelos_doip_core *doip, int s, int64_t now)
{
	struct zelos_doip_socket *sock = &doip->socket[s];
	const uint16_t type = sys_get_be16(&sock->buf[2]);
	const size_t len = sys_get_be32(&sock->buf[4]);
	const uint8_t *p = &sock->buf[ZELOS_DOIP_HEADER_SIZE];
	bool valid;

	switch (type) {
	case ACTIVATE:
		valid = len == 7 || len == 11;
		break;
	case ALIVE_CHECK:
		valid = len == 0;
		break;
	case ALIVE_CHECK_RESPONSE:
		valid = len == 2;
		break;
	case DIAGNOSTIC:
		valid = len >= 5;
		break;
	default:
		send_nack(doip, s, UNKNOWN_TYPE);
		return;
	}
	if (!valid) {
		send_nack(doip, s, INVALID_LENGTH);
		zelos_doip_core_close(doip, s);
		return;
	}
	if (sock->state == ZELOS_DOIP_ACTIVE) {
		sock->idle_until = now + GENERAL_INACTIVITY_MS;
	}

	switch (type) {
	case ACTIVATE:
		sock->activate_source = sys_get_be16(&p[0]);
		sock->activate_type = p[2];
		activate(doip, s, now);
		break;
	case ALIVE_CHECK:
		/* A tester checking on the entity. */
		sys_put_be16(doip->config->logical_address, &doip->out[ZELOS_DOIP_HEADER_SIZE]);
		send(doip, s, header(doip->out, ALIVE_CHECK_RESPONSE, 2));
		break;
	case ALIVE_CHECK_RESPONSE:
		if (sock->alive_wait) {
			sock->alive_wait = false;
			sock->alive = true;
			resume(doip, now);
		}
		break;
	default:
		diagnostic(doip, s, p, len);
		break;
	}
}

/* Bytes of the message in buf to read: all of it, or of an oversized one, its addresses. */
static size_t wanted(const struct zelos_doip_socket *sock)
{
	uint32_t size;

	if (sock->have < ZELOS_DOIP_HEADER_SIZE) {
		return ZELOS_DOIP_HEADER_SIZE;
	}
	size = sys_get_be32(&sock->buf[4]);
	if (size <= ZELOS_DOIP_MDS) {
		return ZELOS_DOIP_HEADER_SIZE + size;
	}
	return ZELOS_DOIP_HEADER_SIZE + (sys_get_be16(&sock->buf[2]) == DIAGNOSTIC ? 4 : 0);
}

void zelos_doip_core_receive(struct zelos_doip_core *doip, int s, const uint8_t *data, size_t len,
			     int64_t now)
{
	struct zelos_doip_socket *sock = &doip->socket[s];

	while (len > 0 && sock->state != ZELOS_DOIP_FREE) {
		size_t n;

		if (sock->drop > 0) {
			n = MIN(len, sock->drop);
			sock->drop -= n;
			data += n;
			len -= n;
			continue;
		}
		n = MIN(len, wanted(sock) - sock->have);
		memcpy(&sock->buf[sock->have], data, n);
		sock->have += n;
		data += n;
		len -= n;
		if (sock->have == ZELOS_DOIP_HEADER_SIZE && n > 0 && !version_ok(sock->buf, false)) {
			/* The header just completed: a bad one desynchronises the stream. */
			send_nack(doip, s, INCORRECT_PATTERN);
			zelos_doip_core_close(doip, s);
			return;
		}
		if (sock->have < wanted(sock)) {
			continue;
		}
		if (sys_get_be32(&sock->buf[4]) > ZELOS_DOIP_MDS) {
			/* Read only far enough to answer; the rest is dropped unread. */
			const uint8_t *p = &sock->buf[ZELOS_DOIP_HEADER_SIZE];
			const size_t read = sock->have - ZELOS_DOIP_HEADER_SIZE;

			if (read == 4) {
				diagnostic_nack(doip, s, sys_get_be16(&p[0]), sys_get_be16(&p[2]),
						MESSAGE_TOO_LARGE);
			} else {
				send_nack(doip, s, TOO_LARGE);
			}
			sock->drop = sys_get_be32(&sock->buf[4]) - read;
			sock->have = 0;
			continue;
		}
		sock->have = 0;
		message(doip, s, now);
	}
}

int64_t zelos_doip_core_tick(struct zelos_doip_core *doip, int64_t now)
{
	int64_t next = FOREVER;
	bool expired = false;

	for (int i = 0; i < ZELOS_DOIP_SOCKETS; i++) {
		struct zelos_doip_socket *sock = &doip->socket[i];

		if (sock->state == ZELOS_DOIP_FREE) {
			continue;
		}
		if (sock->alive_wait && now >= sock->alive_until) {
			/* Its tester is gone. */
			sock->alive_wait = false;
			zelos_doip_core_close(doip, i);
			expired = true;
			continue;
		}
		if (now >= sock->idle_until) {
			zelos_doip_core_close(doip, i);
			continue;
		}
		next = MIN(next, sock->idle_until);
		if (sock->alive_wait) {
			next = MIN(next, sock->alive_until);
		}
	}
	if (expired) {
		resume(doip, now);
	}
	return next;
}

size_t zelos_doip_core_announcement(struct zelos_doip_core *doip, uint8_t *out)
{
	uint8_t *p = &out[ZELOS_DOIP_HEADER_SIZE];

	doip->config->vin(p);
	sys_put_be16(doip->config->logical_address, &p[17]);
	memcpy(&p[19], doip->eid, 6);
	/* GID = EID: no group. */
	memcpy(&p[25], doip->eid, 6);
	/* Further action: none. */
	p[31] = 0x00;
	return header(out, ANNOUNCEMENT, 32);
}

size_t zelos_doip_core_udp(struct zelos_doip_core *doip, const uint8_t *in, size_t len,
			   uint8_t *out, bool *delay)
{
	uint8_t *p = &out[ZELOS_DOIP_HEADER_SIZE];
	uint8_t vin[ZELOS_DOIP_VIN_SIZE];
	uint16_t type;
	uint32_t size;

	*delay = false;
	if (len < ZELOS_DOIP_HEADER_SIZE) {
		return 0;
	}
	type = sys_get_be16(&in[2]);
	size = sys_get_be32(&in[4]);
	if (!version_ok(in, type >= IDENTIFY && type <= IDENTIFY_VIN)) {
		return nack(out, INCORRECT_PATTERN);
	}
	if (size > ZELOS_DOIP_MDS) {
		return nack(out, TOO_LARGE);
	}
	switch (type) {
	case IDENTIFY:
	case ENTITY_STATUS:
	case POWER_MODE:
		if (size != 0) {
			return nack(out, INVALID_LENGTH);
		}
		break;
	case IDENTIFY_EID:
		if (size != 6) {
			return nack(out, INVALID_LENGTH);
		}
		break;
	case IDENTIFY_VIN:
		if (size != ZELOS_DOIP_VIN_SIZE) {
			return nack(out, INVALID_LENGTH);
		}
		break;
	default:
		return nack(out, UNKNOWN_TYPE);
	}
	if (len != ZELOS_DOIP_HEADER_SIZE + size) {
		return nack(out, INVALID_LENGTH);
	}
	in += ZELOS_DOIP_HEADER_SIZE;

	switch (type) {
	case ENTITY_STATUS:
		p[0] = NODE;
		p[1] = ZELOS_DOIP_TESTERS;
		p[2] = zelos_doip_core_testers(doip);
		sys_put_be32(ZELOS_DOIP_MDS, &p[3]);
		return header(out, ENTITY_STATUS_RESPONSE, 7);
	case POWER_MODE:
		p[0] = POWER_READY;
		return header(out, POWER_MODE_RESPONSE, 1);
	case IDENTIFY_EID:
		if (memcmp(in, doip->eid, 6) != 0) {
			return 0;
		}
		break;
	case IDENTIFY_VIN:
		doip->config->vin(vin);
		if (memcmp(in, vin, sizeof(vin)) != 0) {
			return 0;
		}
		break;
	default:
		break;
	}
	*delay = true;
	return zelos_doip_core_announcement(doip, out);
}
