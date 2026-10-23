/*
 * DoIP entity (ISO 13400-2:2012) for the node's UDS server.
 *
 * UDP 13400: vehicle identification (plain, by EID, by VIN) answered after a
 * random 0-500 ms, entity status, power mode; three vehicle announcements, 500
 * ms apart, after the address comes up. TCP 13400: routing activation, alive
 * check, diagnostic messages into zelos_uds_serve() with the tester's logical
 * address as the origin, so the one UDS core arbitrates CAN and DoIP testers
 * (see zelos_uds_core_serve()). No TLS, no central security.
 *
 * Two testers at once (MCTS 2). Routing activation answers 0x10, or refuses
 * and closes: 0x00 a source address outside 0x0E00-0x0FFF, 0x06 an activation
 * type other than 0x00 or 0xE0, 0x02 another address than the one this socket
 * activated, 0x03 an address active on another socket that answers an alive
 * check within 500 ms, 0x01 both sockets taken by testers that answer one. A
 * socket that does not activate within 2 s, or is silent for 300 s once
 * activated, is closed; so is every socket when the link goes down. A closed
 * socket's tester gives up the UDS session it held (zelos_uds_release()).
 *
 * Sends protocol version 0x02; takes 0x02 and 0x03, and 0xFF in vehicle
 * identification requests. A diagnostic message is acknowledged (0x8002) at
 * once; the response follows as its own diagnostic message.
 */

#ifndef ZELOS_DOIP_H_
#define ZELOS_DOIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZELOS_DOIP_PORT 13400
#define ZELOS_DOIP_HEADER_SIZE 8
/* Testers at once (MCTS), and one more socket to refuse a third with. */
#define ZELOS_DOIP_TESTERS 2
#define ZELOS_DOIP_SOCKETS (ZELOS_DOIP_TESTERS + 1)
/* Largest diagnostic message payload (MDS): source, target, a full UDS message. */
#define ZELOS_DOIP_MDS (4 + CONFIG_ZELOS_UDS_BUF_SIZE)
/* Largest message the entity sends or takes. */
#define ZELOS_DOIP_MESSAGE_MAX (ZELOS_DOIP_HEADER_SIZE + ZELOS_DOIP_MDS)
#define ZELOS_DOIP_VIN_SIZE 17

/* The entity's identity. */
struct zelos_doip_config {
	uint16_t logical_address;
	uint16_t functional_address;
	/* The VIN as it is now (UDS DID 0xF190). */
	void (*vin)(uint8_t out[ZELOS_DOIP_VIN_SIZE]);
};

#if defined(CONFIG_ZELOS_DOIP)

/**
 * Start the entity: it waits for lib/eth's address, then announces itself.
 * Call once, after zelos_uds_start(). The configuration must outlive the node.
 */
int zelos_doip_start(const struct zelos_doip_config *config);

/* Activated tester sockets now. */
uint8_t zelos_doip_testers(void);

#endif /* CONFIG_ZELOS_DOIP */

/*
 * The protocol core, free of I/O: bytes in, messages out through ops. `now`
 * is milliseconds on any monotonic clock. One thread drives it.
 */

struct zelos_doip_ops {
	/* Send one message on socket s. False: it failed, and the core closes s. */
	bool (*send)(void *ctx, int s, const uint8_t *msg, size_t len);
	/* Close socket s; its state is still readable (zelos_doip_core.socket[s]). */
	void (*close)(void *ctx, int s);
	/* A UDS request from tester; the response, if any, into rsp. */
	size_t (*diagnose)(void *ctx, uint16_t tester, const uint8_t *req, size_t len,
			   bool functional, uint8_t *rsp);
};

enum zelos_doip_state {
	ZELOS_DOIP_FREE,
	ZELOS_DOIP_OPEN,
	ZELOS_DOIP_ACTIVE,
};

struct zelos_doip_socket {
	uint8_t state;
	/* Its activated tester (ACTIVE). */
	uint16_t tester;
	/* Closed at this time unless traffic moves it (initial or general inactivity). */
	int64_t idle_until;
	/* An alive check sent, unanswered until alive_until; answered this round. */
	bool alive_wait;
	int64_t alive_until;
	bool alive;
	/* A routing activation waiting on alive checks: its source and type. */
	bool activating;
	uint16_t activate_source;
	uint8_t activate_type;
	/* The message being received: bytes so far; bytes of an oversized one to drop. */
	size_t have;
	size_t drop;
	uint8_t buf[ZELOS_DOIP_MESSAGE_MAX];
};

struct zelos_doip_core {
	const struct zelos_doip_config *config;
	const struct zelos_doip_ops *ops;
	void *ctx;
	uint8_t eid[6];
	struct zelos_doip_socket socket[ZELOS_DOIP_SOCKETS];
	uint8_t out[ZELOS_DOIP_MESSAGE_MAX];
};

/* eid: the EID and GID, the MAC. */
void zelos_doip_core_init(struct zelos_doip_core *doip, const struct zelos_doip_config *config,
			  const uint8_t eid[6], const struct zelos_doip_ops *ops, void *ctx);

/** A TCP connection came in. @return its socket index, or -1: none free, close it. */
int zelos_doip_core_accept(struct zelos_doip_core *doip, int64_t now);

/** Bytes received on socket s, any split. */
void zelos_doip_core_receive(struct zelos_doip_core *doip, int s, const uint8_t *data, size_t len,
			     int64_t now);

/** Close socket s (the peer closed it, the link went down): ops->close, then free. */
void zelos_doip_core_close(struct zelos_doip_core *doip, int s);

/** Run the timers. @return When to call again at the latest. */
int64_t zelos_doip_core_tick(struct zelos_doip_core *doip, int64_t now);

/**
 * One UDP datagram. @return Length of the answer written to out, 0: none.
 * *delay: a vehicle identification response, to send after A_DoIP_Announce_Wait.
 */
size_t zelos_doip_core_udp(struct zelos_doip_core *doip, const uint8_t *in, size_t len,
			   uint8_t *out, bool *delay);

/** A vehicle announcement into out. @return its length. */
size_t zelos_doip_core_announcement(struct zelos_doip_core *doip, uint8_t *out);

/** A diagnostic message from the entity to an activated tester, e.g. a pending response. */
void zelos_doip_core_respond(struct zelos_doip_core *doip, uint16_t tester, const uint8_t *rsp,
			     size_t len);

/* Activated sockets. */
uint8_t zelos_doip_core_testers(const struct zelos_doip_core *doip);

#endif /* ZELOS_DOIP_H_ */
