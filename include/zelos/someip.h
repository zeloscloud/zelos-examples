/*
 * SOME/IP server with Service Discovery (AUTOSAR PRS SOME/IP and SOME/IP-SD,
 * R22-11), for one service instance on UDP.
 *
 * SD on UDP 30490, multicast group ZELOS_SOMEIP_SD_GROUP: after the address
 * comes up, a random 10-100 ms initial wait, three offers 100, 200 and 400 ms
 * apart, then one every second, TTL 3 s, with the service's UDP endpoint. A
 * FindService for it is answered by a unicast offer within 100 ms. A
 * SubscribeEventgroup to its eventgroup, with a UDP endpoint option, is
 * acknowledged and the subscriber gets the events, unicast, until its TTL ends
 * or it stops; anything else gets a negative acknowledgement. Four
 * subscribers at most. A link down forgets them; SD starts over at link up.
 * zelos_reboot() sends a StopOffer first. The Reboot flag stays set until a
 * session counter wraps, so peers can tell a reboot.
 *
 * The service: cyclic events, sent to subscribers from the first subscription;
 * fields with a getter and a notifier, the notifier sent to a new subscriber at
 * once and to all on every change. No setters, no methods with side effects.
 * While the node may not send its normal messages (UDS 0x28), events and
 * notifications stop; SD and getters go on.
 *
 * Big endian, as the PRS has it. No SOME/IP-TP, no TCP, no E2E.
 */

#ifndef ZELOS_SOMEIP_H_
#define ZELOS_SOMEIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZELOS_SOMEIP_SD_PORT 30490
/* 224.224.224.245. */
#define ZELOS_SOMEIP_SD_GROUP 0xE0E0E0F5U
#define ZELOS_SOMEIP_HEADER_SIZE 16
#define ZELOS_SOMEIP_SUBSCRIBERS 4
#define ZELOS_SOMEIP_EVENTS_MAX 4
#define ZELOS_SOMEIP_FIELDS_MAX 4
/* Largest event or field payload. */
#define ZELOS_SOMEIP_PAYLOAD_MAX 64
#define ZELOS_SOMEIP_MESSAGE_MAX 256

struct zelos_someip_event {
	uint16_t id;
	uint16_t cycle_ms;
	uint8_t size;
	/* The latest value into out; false: none yet. */
	bool (*read)(uint8_t *out);
};

struct zelos_someip_field {
	uint16_t getter;
	uint16_t notifier;
	uint8_t size;
	void (*read)(uint8_t *out);
};

struct zelos_someip_service {
	uint16_t service;
	uint16_t instance;
	uint8_t major;
	uint32_t minor;
	uint16_t port;
	uint16_t eventgroup;
	const struct zelos_someip_event *events;
	size_t event_count;
	const struct zelos_someip_field *fields;
	size_t field_count;
	/* Whether events and notifications may go out now. */
	bool (*may_notify)(void);
};

#if defined(CONFIG_ZELOS_SOMEIP)

/**
 * Offer the service: SD starts once lib/eth's address is up. Call once. The
 * service must outlive the node.
 */
int zelos_someip_offer(const struct zelos_someip_service *service);

/* Subscribers now. */
uint8_t zelos_someip_subscribers(void);

#endif /* CONFIG_ZELOS_SOMEIP */

/*
 * The protocol core, free of I/O. Addresses and ports in host order;
 * address 0 is the SD multicast group. `now` in ms. One thread drives it.
 */

struct zelos_someip_ops {
	/* A message from the SD port. */
	void (*send_sd)(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len);
	/* A message from the service's port. */
	void (*send)(void *ctx, uint32_t addr, uint16_t port, const uint8_t *msg, size_t len);
	uint32_t (*random)(void);
};

struct zelos_someip_subscriber {
	bool used;
	uint32_t addr;
	uint16_t port;
	int64_t until;
};

struct zelos_someip_core {
	const struct zelos_someip_service *service;
	const struct zelos_someip_ops *ops;
	void *ctx;
	uint32_t addr;
	bool up;
	/* SD: repetitions sent (0..3, then main phase), when the next offer goes. */
	uint8_t repetition;
	int64_t next_offer;
	/* A unicast offer answering a FindService, and when. */
	bool find_reply;
	uint32_t find_addr;
	uint16_t find_port;
	int64_t find_at;
	/* Session IDs, multicast and unicast SD and the service; Reboot flags. */
	uint16_t session_mc;
	uint16_t session_uc;
	uint16_t session;
	bool reboot_mc;
	bool reboot_uc;
	struct zelos_someip_subscriber subscriber[ZELOS_SOMEIP_SUBSCRIBERS];
	int64_t next_event[ZELOS_SOMEIP_EVENTS_MAX];
	uint8_t field[ZELOS_SOMEIP_FIELDS_MAX][ZELOS_SOMEIP_PAYLOAD_MAX];
	uint8_t out[ZELOS_SOMEIP_MESSAGE_MAX];
};

void zelos_someip_core_init(struct zelos_someip_core *sip, const struct zelos_someip_service *svc,
			    const struct zelos_someip_ops *ops, void *ctx);

/** The address is up (addr, host order): SD starts from its initial wait. */
void zelos_someip_core_up(struct zelos_someip_core *sip, uint32_t addr, int64_t now);

/** The link went down: subscribers forgotten, nothing sent until up again. */
void zelos_someip_core_down(struct zelos_someip_core *sip);

/** A datagram on the SD port. */
void zelos_someip_core_sd(struct zelos_someip_core *sip, uint32_t addr, uint16_t port,
			  const uint8_t *data, size_t len, int64_t now);

/** A datagram on the service's port: a getter, answered through ops->send. */
void zelos_someip_core_request(struct zelos_someip_core *sip, uint32_t addr, uint16_t port,
			       const uint8_t *data, size_t len);

/** Offers, events, notifications, TTLs. @return When to call again at the latest. */
int64_t zelos_someip_core_tick(struct zelos_someip_core *sip, int64_t now);

/** A StopOffer into out, multicast. @return its length; 0 while down. */
size_t zelos_someip_core_stop_offer(struct zelos_someip_core *sip, uint8_t *out);

uint8_t zelos_someip_core_subscribers(const struct zelos_someip_core *sip);

#endif /* ZELOS_SOMEIP_H_ */
