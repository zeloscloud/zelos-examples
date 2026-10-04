/*
 * XCP on CAN slave.
 *
 * Lets a calibration tool (an XCP master) read a node's variables, write its
 * calibration parameters and sample variables periodically (DAQ), all over the
 * node's existing CAN bus.
 *
 * The master can only reach memory the application registers: every read,
 * write and DAQ entry is checked against those regions, so a tool on the bus
 * cannot read or overwrite anything else. No seed/key, no flash programming.
 *
 * Fixed for classic CAN: MAX_CTO = MAX_DTO = 8, Intel byte order, byte
 * addressing, address extension 0, dynamic DAQ with absolute ODT numbers.
 * A DAQ list may carry a 4-byte, 1 us timestamp in its first ODT; an overload
 * drops whole samples and is reported with EV_DAQ_OVERLOAD.
 */

#ifndef ZELOS_XCP_H_
#define ZELOS_XCP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One block of memory the master may access. */
struct zelos_xcp_region {
	void *ptr;
	uint32_t size;
	/* Calibration: DOWNLOAD may write here. Reads are always allowed. */
	bool writable;
};

/* One point in the application's loop where DAQ lists can sample. */
struct zelos_xcp_event_channel {
	const char *name;
	/* Nominal period, reported to the master. */
	uint8_t period_ms;
};

struct zelos_xcp_config {
	const struct zelos_xcp_region *regions;
	size_t region_count;
	const struct zelos_xcp_event_channel *events;
	size_t event_count;
	/* GET_ID answer: the A2L file name without path or extension. */
	const char *id;
	/* The build's EPK: readable by the master, ADDR_EPK in the A2L. NULL: none. */
	const char *epk;
};

#if defined(CONFIG_ZELOS_XCP)

/* This build's EPK; the A2L generated beside the ELF quotes it. */
extern const char zelos_xcp_epk[];

/**
 * Start answering the master on the configured CRO/DTO identifiers.
 *
 * Call once, after zelos_can_init(). The configuration must outlive the node.
 *
 * @return 0 on success, negative errno otherwise.
 */
int zelos_xcp_start(const struct zelos_xcp_config *config);

/**
 * Sample every running DAQ list on this event channel.
 *
 * Call from the application's thread at the point its variables are
 * consistent. Copies the sampled bytes and returns; transmission happens on
 * the XCP thread, so a busy bus never stalls the caller.
 *
 * @param channel Index into the configured event channels.
 */
void zelos_xcp_event(uint8_t channel);

#endif /* CONFIG_ZELOS_XCP */

/*
 * The protocol core, free of I/O and Zephyr calls: frames in, frames out.
 * Every frame the slave sends goes on the DTO identifier, so a frame here is
 * only its payload. `now` is the DAQ clock: 1 us per tick, wrapping at 32 bits.
 */

#define ZELOS_XCP_MAX_CTO 8
#define ZELOS_XCP_MAX_DTO 8

struct zelos_xcp_frame {
	uint8_t len;
	uint8_t data[ZELOS_XCP_MAX_DTO];
};

struct zelos_xcp_daq_list {
	uint8_t first_odt;
	uint8_t odt_count;
	uint8_t channel;
	uint8_t prescaler;
	uint8_t countdown;
	bool timestamp;
	bool selected;
	bool running;
};

struct zelos_xcp_odt {
	uint16_t first_entry;
	uint8_t entry_count;
};

struct zelos_xcp_odt_entry {
	/* Resolved against the regions when written, so sampling needs no check. */
	const uint8_t *ptr;
	uint8_t size;
};

/* Room for one sample of every ODT, plus a command response. */
#define ZELOS_XCP_TX_QUEUE (CONFIG_ZELOS_XCP_ODTS + 1)

struct zelos_xcp_core {
	const struct zelos_xcp_config *config;
	bool connected;
	uint32_t mta;

	struct zelos_xcp_daq_list daq[CONFIG_ZELOS_XCP_DAQ_LISTS];
	struct zelos_xcp_odt odt[CONFIG_ZELOS_XCP_ODTS];
	struct zelos_xcp_odt_entry entry[CONFIG_ZELOS_XCP_ODT_ENTRIES];
	uint8_t daq_count;
	uint8_t odt_count;
	uint16_t entry_count;

	/* SET_DAQ_PTR target, advanced by each WRITE_DAQ. */
	uint16_t ptr_daq;
	uint8_t ptr_odt;
	uint8_t ptr_entry;

	struct zelos_xcp_frame tx[ZELOS_XCP_TX_QUEUE];
	uint8_t tx_head;
	uint8_t tx_len;
	/* A sample was dropped; EV_DAQ_OVERLOAD goes out once the queue drains. */
	bool overload;
};

void zelos_xcp_core_init(struct zelos_xcp_core *xcp, const struct zelos_xcp_config *config);

/* Handle one CRO. Its response, if any, is queued for zelos_xcp_core_poll(). */
void zelos_xcp_core_on_frame(struct zelos_xcp_core *xcp, const uint8_t *data, uint8_t len,
			     uint32_t now);

/* Queue one DTO per ODT of every running DAQ list on this channel that is due. */
void zelos_xcp_core_event(struct zelos_xcp_core *xcp, uint8_t channel, uint32_t now);

/* Take the next frame to send. Returns false when there is none. */
bool zelos_xcp_core_poll(struct zelos_xcp_core *xcp, struct zelos_xcp_frame *out);

#endif /* ZELOS_XCP_H_ */
