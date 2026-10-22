/* SOME/IP + SD server core (AUTOSAR PRS R22-11). No I/O: see include/zelos/someip.h. */

#include <zelos/someip.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define PROTOCOL_VERSION 0x01
#define SD_SERVICE 0xFFFF
#define SD_METHOD 0x8100

enum {
	REQUEST = 0x00,
	REQUEST_NO_RETURN = 0x01,
	NOTIFICATION = 0x02,
	RESPONSE = 0x80,
	ERROR = 0x81,
};

enum {
	E_OK = 0x00,
	E_UNKNOWN_SERVICE = 0x02,
	E_UNKNOWN_METHOD = 0x03,
	E_WRONG_PROTOCOL_VERSION = 0x07,
	E_WRONG_INTERFACE_VERSION = 0x08,
	E_MALFORMED_MESSAGE = 0x09,
};

enum {
	FIND_SERVICE = 0x00,
	OFFER_SERVICE = 0x01,
	SUBSCRIBE = 0x06,
	SUBSCRIBE_ACK = 0x07,
};

#define FLAG_REBOOT 0x80
#define FLAG_UNICAST 0x40
#define ENTRY_SIZE 16
#define OPTION_IPV4_ENDPOINT 0x04
#define OPTION_IPV4_SIZE 12
#define UDP 0x11
#define TTL_INFINITE 0xFFFFFF

/* PRS_SOMEIPSD_00416's example timing; TTL three cycles. */
#define INITIAL_DELAY_MIN_MS 10
#define INITIAL_DELAY_MAX_MS 100
#define REPETITIONS_BASE_DELAY_MS 100
#define REPETITIONS_MAX 3
#define CYCLIC_OFFER_DELAY_MS 1000
#define REQUEST_RESPONSE_DELAY_MAX_MS 100
#define TTL_S 3

#define FOREVER INT64_MAX

static uint16_t next_session(uint16_t *session, bool *reboot)
{
	/* 1..0xFFFF; the Reboot flag clears at the first wrap (PRS_SOMEIPSD_00255). */
	if (*session == 0xFFFF) {
		*session = 1;
		*reboot = false;
	} else {
		(*session)++;
	}
	return *session;
}

static size_t header(uint8_t *out, uint16_t service, uint16_t method, uint16_t client,
		     uint16_t session, uint8_t version, uint8_t type, uint8_t code, size_t payload)
{
	sys_put_be16(service, &out[0]);
	sys_put_be16(method, &out[2]);
	sys_put_be32(8 + payload, &out[4]);
	sys_put_be16(client, &out[8]);
	sys_put_be16(session, &out[10]);
	out[12] = PROTOCOL_VERSION;
	out[13] = version;
	out[14] = type;
	out[15] = code;
	return ZELOS_SOMEIP_HEADER_SIZE + payload;
}

void zelos_someip_core_init(struct zelos_someip_core *sip, const struct zelos_someip_service *svc,
			    const struct zelos_someip_ops *ops, void *ctx)
{
	memset(sip, 0, sizeof(*sip));
	sip->service = svc;
	sip->ops = ops;
	sip->ctx = ctx;
	sip->reboot_mc = true;
}

uint8_t zelos_someip_core_subscribers(const struct zelos_someip_core *sip)
{
	uint8_t n = 0;

	for (int i = 0; i < ZELOS_SOMEIP_SUBSCRIBERS; i++) {
		n += sip->subscriber[i].used;
	}
	return n;
}

void zelos_someip_core_up(struct zelos_someip_core *sip, uint32_t addr, int64_t now)
{
	sip->addr = addr;
	sip->up = true;
	sip->repetition = 0;
	sip->find_reply = false;
	sip->next_offer = now + INITIAL_DELAY_MIN_MS +
			  sip->ops->random() % (INITIAL_DELAY_MAX_MS - INITIAL_DELAY_MIN_MS + 1);
}

void zelos_someip_core_down(struct zelos_someip_core *sip)
{
	sip->up = false;
	memset(sip->subscriber, 0, sizeof(sip->subscriber));
}

/* The next session ID to addr (0: multicast), and its Reboot flag. */
static uint16_t session_to(struct zelos_someip_core *sip, uint32_t addr, bool *reboot)
{
	struct zelos_someip_peer *p = NULL;
	uint16_t session;

	if (addr == 0) {
		session = next_session(&sip->session_mc, &sip->reboot_mc);
		*reboot = sip->reboot_mc;
		return session;
	}
	for (int i = 0; i < ZELOS_SOMEIP_PEERS; i++) {
		struct zelos_someip_peer *q = &sip->peer[i];

		if (q->used != 0 && q->addr == addr) {
			p = q;
			break;
		}
		if (p == NULL || q->used < p->used) {
			p = q;
		}
	}
	if (p->used == 0 || p->addr != addr) {
		*p = (struct zelos_someip_peer){.addr = addr, .reboot = true};
	}
	p->used = ++sip->peer_clock;
	session = next_session(&p->session, &p->reboot);
	*reboot = p->reboot;
	return session;
}

/* An SD message to addr (0: multicast) with one offer entry (TTL ttl) and its endpoint. */
static size_t offer(struct zelos_someip_core *sip, uint8_t *out, uint32_t addr, uint32_t ttl)
{
	const struct zelos_someip_service *svc = sip->service;
	uint8_t *p = &out[ZELOS_SOMEIP_HEADER_SIZE];
	bool reboot;
	uint16_t session = session_to(sip, addr, &reboot);

	p[0] = (reboot ? FLAG_REBOOT : 0) | FLAG_UNICAST;
	p[1] = p[2] = p[3] = 0;
	sys_put_be32(ENTRY_SIZE, &p[4]);
	p += 8;
	p[0] = OFFER_SERVICE;
	p[1] = 0;
	p[2] = 0;
	p[3] = 1 << 4;
	sys_put_be16(svc->service, &p[4]);
	sys_put_be16(svc->instance, &p[6]);
	p[8] = svc->major;
	sys_put_be24(ttl, &p[9]);
	sys_put_be32(svc->minor, &p[12]);
	p += ENTRY_SIZE;
	sys_put_be32(OPTION_IPV4_SIZE, &p[0]);
	p += 4;
	sys_put_be16(OPTION_IPV4_SIZE - 3, &p[0]);
	p[2] = OPTION_IPV4_ENDPOINT;
	p[3] = 0;
	sys_put_be32(sip->addr, &p[4]);
	p[8] = 0;
	p[9] = UDP;
	sys_put_be16(svc->port, &p[10]);
	return header(out, SD_SERVICE, SD_METHOD, 0, session, 0x01, NOTIFICATION, E_OK,
		      8 + ENTRY_SIZE + 4 + OPTION_IPV4_SIZE);
}

size_t zelos_someip_core_stop_offer(struct zelos_someip_core *sip, uint8_t *out)
{
	return sip->up ? offer(sip, out, 0, 0) : 0;
}

static void send_value(struct zelos_someip_core *sip, const struct zelos_someip_subscriber *to,
		       uint16_t event, const uint8_t *value, size_t size)
{
	size_t len;

	sip->session = sip->session == 0xFFFF ? 1 : sip->session + 1;
	len = header(sip->out, sip->service->service, event, 0, sip->session, sip->service->major,
		     NOTIFICATION, E_OK, size);

	memcpy(&sip->out[ZELOS_SOMEIP_HEADER_SIZE], value, size);
	sip->ops->send(sip->ctx, to->addr, to->port, sip->out, len);
}

static void notify_all(struct zelos_someip_core *sip, uint16_t event, const uint8_t *value,
		       size_t size)
{
	for (int i = 0; i < ZELOS_SOMEIP_SUBSCRIBERS; i++) {
		if (sip->subscriber[i].used) {
			send_value(sip, &sip->subscriber[i], event, value, size);
		}
	}
}

/* The UDP endpoint among the options an entry refers to; false if none. */
static bool endpoint(const uint8_t *entry, const uint8_t *options, size_t options_len,
		     uint32_t *addr, uint16_t *port)
{
	const uint8_t first[2] = {entry[1], entry[2]};
	const uint8_t count[2] = {entry[3] >> 4, entry[3] & 0x0F};

	for (int run = 0; run < 2; run++) {
		for (int k = 0; k < count[run]; k++) {
			const uint8_t *o = options;
			size_t left = options_len;

			/* Walk to option first[run] + k, each length read only where it is. */
			for (int i = 0; i < first[run] + k; i++) {
				size_t size;

				if (left < 3) {
					return false;
				}
				size = 3 + sys_get_be16(o);
				if (size > left) {
					return false;
				}
				o += size;
				left -= size;
			}
			if (left >= OPTION_IPV4_SIZE && o[2] == OPTION_IPV4_ENDPOINT &&
			    sys_get_be16(o) == OPTION_IPV4_SIZE - 3 && o[9] == UDP) {
				*addr = sys_get_be32(&o[4]);
				*port = sys_get_be16(&o[10]);
				return true;
			}
		}
	}
	return false;
}

/* Answer one SubscribeEventgroup entry: Ack (ttl kept) or Nack (ttl 0). */
static void subscribe(struct zelos_someip_core *sip, const uint8_t *entry, const uint8_t *options,
		      size_t options_len, uint32_t from, uint16_t from_port, int64_t now)
{
	const struct zelos_someip_service *svc = sip->service;
	const uint32_t ttl = sys_get_be24(&entry[9]);
	uint8_t *p = &sip->out[ZELOS_SOMEIP_HEADER_SIZE];
	struct zelos_someip_subscriber *slot = NULL;
	bool ok = sys_get_be16(&entry[4]) == svc->service &&
		  sys_get_be16(&entry[6]) == svc->instance && entry[8] == svc->major &&
		  sys_get_be16(&entry[14]) == svc->eventgroup;
	uint32_t addr = 0;
	uint16_t port = 0;
	bool fresh = false;
	bool reboot;
	uint16_t session;
	size_t len;

	/* 0.0.0.0 or port 0 would send nowhere, or to the SD group. */
	ok = ok && endpoint(entry, options, options_len, &addr, &port) && addr != 0 && port != 0;
	if (ok) {
		for (int i = 0; i < ZELOS_SOMEIP_SUBSCRIBERS; i++) {
			struct zelos_someip_subscriber *s = &sip->subscriber[i];

			if (s->used && s->addr == addr && s->port == port) {
				slot = s;
				break;
			}
			if (!s->used && slot == NULL) {
				slot = s;
			}
		}
		if (ttl == 0) {
			/* StopSubscribeEventgroup: no answer. */
			if (slot != NULL && slot->used && slot->addr == addr && slot->port == port) {
				slot->used = false;
			}
			return;
		}
		ok = slot != NULL;
	}
	if (ok) {
		fresh = !slot->used;
		slot->used = true;
		slot->addr = addr;
		slot->port = port;
		slot->until = ttl == TTL_INFINITE ? FOREVER : now + (int64_t)ttl * 1000;
	} else if (ttl == 0) {
		return;
	}

	session = session_to(sip, from, &reboot);
	p[0] = (reboot ? FLAG_REBOOT : 0) | FLAG_UNICAST;
	p[1] = p[2] = p[3] = 0;
	sys_put_be32(ENTRY_SIZE, &p[4]);
	/* The entry back, as an Ack (its TTL) or a Nack (TTL 0), no options. */
	memcpy(&p[8], entry, ENTRY_SIZE);
	p[8] = SUBSCRIBE_ACK;
	p[9] = p[10] = p[11] = 0;
	sys_put_be24(ok ? ttl : 0, &p[8 + 9]);
	sys_put_be32(0, &p[8 + ENTRY_SIZE]);
	len = header(sip->out, SD_SERVICE, SD_METHOD, 0, session, 0x01, NOTIFICATION, E_OK,
		     8 + ENTRY_SIZE + 4);
	sip->ops->send_sd(sip->ctx, from, from_port, sip->out, len);

	if (fresh && svc->may_notify()) {
		/*
		 * A field's notifier sends its value to a new subscriber at once. The
		 * value others last got stays, so a change since still reaches them.
		 */
		for (size_t f = 0; f < svc->field_count; f++) {
			uint8_t value[ZELOS_SOMEIP_PAYLOAD_MAX];

			svc->fields[f].read(value);
			if (zelos_someip_core_subscribers(sip) == 1) {
				memcpy(sip->field[f], value, svc->fields[f].size);
			}
			send_value(sip, slot, svc->fields[f].notifier, value, svc->fields[f].size);
		}
	}
	for (size_t e = 0; e < svc->event_count; e++) {
		if (fresh && zelos_someip_core_subscribers(sip) == 1) {
			sip->next_event[e] = now;
		}
	}
}

void zelos_someip_core_sd(struct zelos_someip_core *sip, uint32_t addr, uint16_t port,
			  const uint8_t *data, size_t len, int64_t now)
{
	const struct zelos_someip_service *svc = sip->service;
	const uint8_t *p = &data[ZELOS_SOMEIP_HEADER_SIZE];
	size_t entries_len;
	size_t options_len;
	size_t avail;
	const uint8_t *options;

	if (!sip->up || len < ZELOS_SOMEIP_HEADER_SIZE + 12 ||
	    sys_get_be16(&data[0]) != SD_SERVICE || sys_get_be16(&data[2]) != SD_METHOD ||
	    sys_get_be32(&data[4]) != len - 8) {
		return;
	}
	/* Lengths from the wire, compared without sums that could wrap. */
	avail = len - ZELOS_SOMEIP_HEADER_SIZE - 12;
	entries_len = sys_get_be32(&p[4]);
	if (entries_len % ENTRY_SIZE != 0 || entries_len > avail) {
		return;
	}
	options_len = sys_get_be32(&p[8 + entries_len]);
	options = &p[8 + entries_len + 4];
	if (options_len > avail - entries_len) {
		return;
	}

	for (size_t at = 0; at < entries_len; at += ENTRY_SIZE) {
		const uint8_t *entry = &p[8 + at];
		const uint16_t service = sys_get_be16(&entry[4]);
		const uint16_t instance = sys_get_be16(&entry[6]);

		if (entry[0] == SUBSCRIBE) {
			subscribe(sip, entry, options, options_len, addr, port, now);
		} else if (entry[0] == FIND_SERVICE &&
			   (service == svc->service || service == 0xFFFF) &&
			   (instance == svc->instance || instance == 0xFFFF) &&
			   (entry[8] == svc->major || entry[8] == 0xFF) &&
			   (sys_get_be32(&entry[12]) == svc->minor ||
			    sys_get_be32(&entry[12]) == 0xFFFFFFFF)) {
			sip->find_reply = true;
			sip->find_addr = addr;
			sip->find_port = port;
			sip->find_at = now + sip->ops->random() % (REQUEST_RESPONSE_DELAY_MAX_MS + 1);
		}
	}
}

static void error(struct zelos_someip_core *sip, const uint8_t *req, uint32_t addr, uint16_t port,
		  uint8_t code)
{
	size_t len = header(sip->out, sys_get_be16(&req[0]), sys_get_be16(&req[2]),
			    sys_get_be16(&req[8]), sys_get_be16(&req[10]), req[13], ERROR, code, 0);

	sip->ops->send(sip->ctx, addr, port, sip->out, len);
}

void zelos_someip_core_request(struct zelos_someip_core *sip, uint32_t addr, uint16_t port,
			       const uint8_t *data, size_t len)
{
	const struct zelos_someip_service *svc = sip->service;
	const uint16_t method = len >= ZELOS_SOMEIP_HEADER_SIZE ? sys_get_be16(&data[2]) : 0;

	if (!sip->up || len < ZELOS_SOMEIP_HEADER_SIZE ||
	    (data[14] != REQUEST && data[14] != REQUEST_NO_RETURN)) {
		return;
	}
	if (data[14] == REQUEST_NO_RETURN) {
		/* Fire and forget: nothing here takes one. */
		return;
	}
	if (data[12] != PROTOCOL_VERSION) {
		error(sip, data, addr, port, E_WRONG_PROTOCOL_VERSION);
		return;
	}
	if (sys_get_be32(&data[4]) != len - 8) {
		error(sip, data, addr, port, E_MALFORMED_MESSAGE);
		return;
	}
	if (sys_get_be16(&data[0]) != svc->service) {
		error(sip, data, addr, port, E_UNKNOWN_SERVICE);
		return;
	}
	if (data[13] != svc->major) {
		error(sip, data, addr, port, E_WRONG_INTERFACE_VERSION);
		return;
	}
	for (size_t f = 0; f < svc->field_count; f++) {
		const struct zelos_someip_field *field = &svc->fields[f];
		size_t n;

		if (field->getter != method) {
			continue;
		}
		if (len != ZELOS_SOMEIP_HEADER_SIZE) {
			error(sip, data, addr, port, E_MALFORMED_MESSAGE);
			return;
		}
		n = header(sip->out, svc->service, method, sys_get_be16(&data[8]),
			   sys_get_be16(&data[10]), svc->major, RESPONSE, E_OK, field->size);
		field->read(&sip->out[ZELOS_SOMEIP_HEADER_SIZE]);
		sip->ops->send(sip->ctx, addr, port, sip->out, n);
		return;
	}
	error(sip, data, addr, port, E_UNKNOWN_METHOD);
}

int64_t zelos_someip_core_tick(struct zelos_someip_core *sip, int64_t now)
{
	const struct zelos_someip_service *svc = sip->service;
	const bool notify = svc->may_notify();
	uint8_t value[ZELOS_SOMEIP_PAYLOAD_MAX];
	int64_t next = FOREVER;
	size_t len;

	if (!sip->up) {
		return next;
	}

	if (now >= sip->next_offer) {
		len = offer(sip, sip->out, 0, TTL_S);
		sip->ops->send_sd(sip->ctx, 0, ZELOS_SOMEIP_SD_PORT, sip->out, len);
		if (sip->repetition < REPETITIONS_MAX) {
			/* 100, 200, 400 ms after the first, then the cycle. */
			sip->next_offer = now + (REPETITIONS_BASE_DELAY_MS << sip->repetition);
			sip->repetition++;
		} else {
			sip->next_offer = now + CYCLIC_OFFER_DELAY_MS;
		}
	}
	next = sip->next_offer;

	if (sip->find_reply) {
		if (now >= sip->find_at) {
			len = offer(sip, sip->out, sip->find_addr, TTL_S);
			sip->ops->send_sd(sip->ctx, sip->find_addr, sip->find_port, sip->out, len);
			sip->find_reply = false;
		} else {
			next = MIN(next, sip->find_at);
		}
	}

	for (int i = 0; i < ZELOS_SOMEIP_SUBSCRIBERS; i++) {
		if (sip->subscriber[i].used && now >= sip->subscriber[i].until) {
			sip->subscriber[i].used = false;
		}
		if (sip->subscriber[i].used) {
			next = MIN(next, sip->subscriber[i].until);
		}
	}
	if (zelos_someip_core_subscribers(sip) == 0) {
		return next;
	}

	for (size_t e = 0; e < svc->event_count; e++) {
		const struct zelos_someip_event *ev = &svc->events[e];

		if (now >= sip->next_event[e]) {
			if (notify && ev->read(value)) {
				notify_all(sip, ev->id, value, ev->size);
			}
			/* On the cycle; after a long stall, from now. */
			sip->next_event[e] += ev->cycle_ms;
			if (sip->next_event[e] <= now) {
				sip->next_event[e] = now + ev->cycle_ms;
			}
		}
		next = MIN(next, sip->next_event[e]);
	}
	for (size_t f = 0; f < svc->field_count; f++) {
		const struct zelos_someip_field *field = &svc->fields[f];

		field->read(value);
		/* A change while notifications are off goes out when they are back. */
		if (notify && memcmp(value, sip->field[f], field->size) != 0) {
			memcpy(sip->field[f], value, field->size);
			notify_all(sip, field->notifier, value, field->size);
		}
	}
	return next;
}
