/*
 * XCP slave protocol core. No I/O, no Zephyr calls: the glue feeds it CROs and
 * events and sends what it queues.
 */

#include <zelos/xcp.h>

#include <string.h>

#include <zephyr/sys/byteorder.h>

enum {
	CMD_CONNECT = 0xFF,
	CMD_DISCONNECT = 0xFE,
	CMD_GET_STATUS = 0xFD,
	CMD_SYNCH = 0xFC,
	CMD_GET_COMM_MODE_INFO = 0xFB,
	CMD_GET_ID = 0xFA,
	CMD_SET_MTA = 0xF6,
	CMD_UPLOAD = 0xF5,
	CMD_SHORT_UPLOAD = 0xF4,
	CMD_DOWNLOAD = 0xF0,
	CMD_SET_DAQ_PTR = 0xE2,
	CMD_WRITE_DAQ = 0xE1,
	CMD_SET_DAQ_LIST_MODE = 0xE0,
	CMD_GET_DAQ_LIST_MODE = 0xDF,
	CMD_START_STOP_DAQ_LIST = 0xDE,
	CMD_START_STOP_SYNCH = 0xDD,
	CMD_GET_DAQ_CLOCK = 0xDC,
	CMD_GET_DAQ_PROCESSOR_INFO = 0xDA,
	CMD_GET_DAQ_RESOLUTION_INFO = 0xD9,
	CMD_GET_DAQ_EVENT_INFO = 0xD7,
	CMD_FREE_DAQ = 0xD6,
	CMD_ALLOC_DAQ = 0xD5,
	CMD_ALLOC_ODT = 0xD4,
	CMD_ALLOC_ODT_ENTRY = 0xD3,
};

enum {
	PID_RES = 0xFF,
	PID_ERR = 0xFE,
	PID_EV = 0xFD,
};

#define EV_DAQ_OVERLOAD 0x06

enum {
	ERR_CMD_SYNCH = 0x00,
	ERR_DAQ_ACTIVE = 0x11,
	ERR_CMD_UNKNOWN = 0x20,
	ERR_CMD_SYNTAX = 0x21,
	ERR_OUT_OF_RANGE = 0x22,
	ERR_WRITE_PROTECTED = 0x23,
	ERR_ACCESS_DENIED = 0x24,
	ERR_MODE_NOT_VALID = 0x27,
	ERR_SEQUENCE = 0x29,
	ERR_DAQ_CONFIG = 0x2A,
	ERR_MEMORY_OVERFLOW = 0x30,
};

/*
 * CONNECT: calibration and DAQ available; no STIM, no programming. CAL/PAG
 * comes without the page commands: it is how a master learns it may
 * calibrate, and the node has only the one page.
 */
#define RESOURCE_CAL_PAG 0x01
#define RESOURCE_DAQ 0x04
/* Intel byte order, byte granularity, GET_COMM_MODE_INFO available. */
#define COMM_MODE_BASIC 0x80
#define SESSION_DAQ_RUNNING 0x40
/*
 * Dynamic configuration, prescaler, timestamps, overload reported by event;
 * absolute ODT numbers as the PID.
 */
#define DAQ_PROPERTIES 0x93
#define DAQ_KEY_BYTE 0x00
/* DAQ only; all lists on one event are copied in one critical section. */
#define EVENT_PROPERTIES 0x84
#define TIME_UNIT_1MS 6
/* 4-byte timestamp, 1 us per tick, selected per DAQ list. */
#define TIMESTAMP_SIZE 4
#define TIMESTAMP_MODE 0x34
/* DAQ list mode bits: TIMESTAMP may be set; STIM, alternating, DTO_CTR, PID_OFF not. */
#define DAQ_MODE_TIMESTAMP 0x10
#define DAQ_MODE_UNSUPPORTED 0x2B
#define DAQ_MODE_SELECTED 0x01
#define DAQ_MODE_RUNNING 0x40

/* One PID byte per DTO; the rest is data. */
#define ODT_PAYLOAD (ZELOS_XCP_MAX_DTO - 1)
/* PIDs from 0xFC up are reserved for responses, errors, events and service. */
_Static_assert(CONFIG_ZELOS_XCP_ODTS <= 0xFC, "absolute ODT numbers must stay below 0xFC");

static uint32_t xcp_addr(const void *ptr)
{
	return (uint32_t)(uintptr_t)ptr;
}

/* True when [addr, addr + len) lies inside [base, base + size). Overflow-safe. */
static bool within(const void *base, uint32_t size, uint32_t addr, uint32_t len)
{
	uint32_t start = xcp_addr(base);

	return addr >= start && len <= size && addr - start <= size - len;
}

/* The string's bytes at [addr, addr + len), or NULL. */
static const uint8_t *in_string(const char *s, uint32_t addr, uint32_t len)
{
	if (s == NULL || !within(s, strlen(s), addr, len)) {
		return NULL;
	}

	return (const uint8_t *)s + (addr - xcp_addr(s));
}

/*
 * The only way from an XCP address to a pointer. Reads may also reach the
 * strings the core itself hands out through the MTA (GET_ID, EPK, event names).
 */
static const uint8_t *resolve(const struct zelos_xcp_core *xcp, uint32_t addr, uint32_t len,
			      bool write)
{
	const struct zelos_xcp_config *cfg = xcp->config;
	const uint8_t *p;

	for (size_t i = 0; i < cfg->region_count; i++) {
		const struct zelos_xcp_region *r = &cfg->regions[i];

		if ((r->writable || !write) && within(r->ptr, r->size, addr, len)) {
			return (const uint8_t *)r->ptr + (addr - xcp_addr(r->ptr));
		}
	}

	if (write) {
		return NULL;
	}

	p = in_string(cfg->id, addr, len);
	if (p == NULL) {
		p = in_string(cfg->epk, addr, len);
	}
	for (size_t i = 0; p == NULL && i < cfg->event_count; i++) {
		p = in_string(cfg->events[i].name, addr, len);
	}

	return p;
}

static struct zelos_xcp_frame *tx_push(struct zelos_xcp_core *xcp)
{
	struct zelos_xcp_frame *f;

	if (xcp->tx_len == ZELOS_XCP_TX_QUEUE) {
		return NULL;
	}

	f = &xcp->tx[(xcp->tx_head + xcp->tx_len) % ZELOS_XCP_TX_QUEUE];
	xcp->tx_len++;
	memset(f, 0, sizeof(*f));

	return f;
}

/* A positive response: 0xFF then len - 1 bytes the caller fills. */
static uint8_t *res(struct zelos_xcp_core *xcp, uint8_t len)
{
	struct zelos_xcp_frame *f = tx_push(xcp);

	if (f == NULL) {
		return NULL;
	}

	f->len = len;
	f->data[0] = PID_RES;

	return f->data;
}

static void err(struct zelos_xcp_core *xcp, uint8_t code)
{
	struct zelos_xcp_frame *f = tx_push(xcp);

	if (f != NULL) {
		f->len = 2;
		f->data[0] = PID_ERR;
		f->data[1] = code;
	}
}

static void daq_stop_all(struct zelos_xcp_core *xcp)
{
	for (uint8_t i = 0; i < xcp->daq_count; i++) {
		xcp->daq[i].running = false;
		xcp->daq[i].selected = false;
	}
	xcp->overload = false;
}

static bool daq_running(const struct zelos_xcp_core *xcp)
{
	for (uint8_t i = 0; i < xcp->daq_count; i++) {
		if (xcp->daq[i].running) {
			return true;
		}
	}

	return false;
}

static void connect(struct zelos_xcp_core *xcp)
{
	uint8_t *r;

	/* A (re)connect starts a clean session: nothing samples until asked. */
	daq_stop_all(xcp);
	xcp->connected = true;

	r = res(xcp, 8);
	if (r != NULL) {
		r[1] = RESOURCE_CAL_PAG | RESOURCE_DAQ;
		r[2] = COMM_MODE_BASIC;
		r[3] = ZELOS_XCP_MAX_CTO;
		r[4] = ZELOS_XCP_MAX_DTO;
		r[5] = 0;
		r[6] = 1; /* protocol layer version */
		r[7] = 1; /* transport layer version */
	}
}

static void get_id(struct zelos_xcp_core *xcp, uint8_t type)
{
	/* Types 0 (ASCII) and 1 (A2L name, no path or extension) share the answer. */
	const char *text = type <= 1 ? xcp->config->id : (type == 5 ? xcp->config->epk : NULL);
	uint8_t *r = res(xcp, 8);

	if (r != NULL) {
		/* Mode 0: the master uploads the text from the MTA. Length 0: not available. */
		sys_put_le32(text != NULL ? strlen(text) : 0, &r[4]);
	}
	if (text != NULL) {
		xcp->mta = xcp_addr(text);
	}
}

static void upload(struct zelos_xcp_core *xcp, uint8_t n)
{
	const uint8_t *src;
	uint8_t *r;

	if (n == 0 || n > ZELOS_XCP_MAX_CTO - 1) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	src = resolve(xcp, xcp->mta, n, false);
	if (src == NULL) {
		err(xcp, ERR_ACCESS_DENIED);
		return;
	}

	r = res(xcp, 1 + n);
	if (r != NULL) {
		/* As download stores it: an aligned 32-bit value in one access. */
		if (n == 4 && ((uintptr_t)src & 3U) == 0U) {
			uint32_t v = *(const volatile uint32_t *)src;

			memcpy(&r[1], &v, 4);
		} else {
			memcpy(&r[1], src, n);
		}
		xcp->mta += n;
	}
}

static void download(struct zelos_xcp_core *xcp, uint8_t n, const uint8_t *data)
{
	uint8_t *dst = (uint8_t *)resolve(xcp, xcp->mta, n, true);

	if (dst == NULL) {
		/* Readable but not writable is protected; anything else is not ours. */
		err(xcp, resolve(xcp, xcp->mta, n, false) != NULL ? ERR_WRITE_PROTECTED
								   : ERR_ACCESS_DENIED);
		return;
	}

	/*
	 * The application reads calibration values without a lock, so an
	 * aligned 32-bit parameter is stored in one access, never byte by byte.
	 */
	if (n == 4 && ((uintptr_t)dst & 3U) == 0U) {
		uint32_t v;

		memcpy(&v, data, 4);
		*(volatile uint32_t *)dst = v;
	} else {
		memcpy(dst, data, n);
	}

	xcp->mta += n;
	(void)res(xcp, 1);
}

static struct zelos_xcp_daq_list *daq_at(struct zelos_xcp_core *xcp, const uint8_t *p)
{
	uint16_t i = sys_get_le16(p);

	return i < xcp->daq_count ? &xcp->daq[i] : NULL;
}

static void free_daq(struct zelos_xcp_core *xcp)
{
	daq_stop_all(xcp);
	xcp->daq_count = 0;
	xcp->odt_count = 0;
	xcp->entry_count = 0;
	(void)res(xcp, 1);
}

/* Allocation runs FREE_DAQ, ALLOC_DAQ, ALLOC_ODT, ALLOC_ODT_ENTRY, in that order. */
static void alloc_daq(struct zelos_xcp_core *xcp, uint16_t count)
{
	if (xcp->daq_count != 0 || xcp->odt_count != 0) {
		err(xcp, ERR_SEQUENCE);
		return;
	}
	if (count > CONFIG_ZELOS_XCP_DAQ_LISTS) {
		err(xcp, ERR_MEMORY_OVERFLOW);
		return;
	}

	memset(xcp->daq, 0, sizeof(xcp->daq));
	for (uint16_t i = 0; i < count; i++) {
		xcp->daq[i].prescaler = 1;
	}
	xcp->daq_count = (uint8_t)count;
	(void)res(xcp, 1);
}

static void alloc_odt(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint8_t count = cmd[4];

	if (d == NULL) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}
	if (d->odt_count != 0 || xcp->entry_count != 0) {
		err(xcp, ERR_SEQUENCE);
		return;
	}
	if (count > CONFIG_ZELOS_XCP_ODTS - xcp->odt_count) {
		err(xcp, ERR_MEMORY_OVERFLOW);
		return;
	}

	d->first_odt = xcp->odt_count;
	d->odt_count = count;
	memset(&xcp->odt[xcp->odt_count], 0, count * sizeof(xcp->odt[0]));
	xcp->odt_count += count;
	(void)res(xcp, 1);
}

static void alloc_odt_entry(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint8_t count = cmd[5];
	struct zelos_xcp_odt *o;

	if (d == NULL || cmd[4] >= d->odt_count) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	o = &xcp->odt[d->first_odt + cmd[4]];
	if (o->entry_count != 0) {
		err(xcp, ERR_SEQUENCE);
		return;
	}
	if (count > CONFIG_ZELOS_XCP_ODT_ENTRIES - xcp->entry_count) {
		err(xcp, ERR_MEMORY_OVERFLOW);
		return;
	}

	o->first_entry = xcp->entry_count;
	o->entry_count = count;
	memset(&xcp->entry[xcp->entry_count], 0, count * sizeof(xcp->entry[0]));
	xcp->entry_count += count;
	(void)res(xcp, 1);
}

static void set_daq_ptr(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);

	if (d == NULL || cmd[4] >= d->odt_count ||
	    cmd[5] >= xcp->odt[d->first_odt + cmd[4]].entry_count) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	xcp->ptr_daq = sys_get_le16(&cmd[2]);
	xcp->ptr_odt = cmd[4];
	xcp->ptr_entry = cmd[5];
	(void)res(xcp, 1);
}

/* Data bytes ODT `odt` of the list can carry: the first one also holds any timestamp. */
static uint32_t odt_room(uint8_t odt, bool timestamp)
{
	return ODT_PAYLOAD - (odt == 0 && timestamp ? TIMESTAMP_SIZE : 0);
}

static uint32_t odt_used(const struct zelos_xcp_core *xcp, const struct zelos_xcp_odt *o)
{
	uint32_t used = 0;

	for (uint8_t i = 0; i < o->entry_count; i++) {
		used += xcp->entry[o->first_entry + i].size;
	}

	return used;
}

static void write_daq(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	uint8_t size = cmd[2];
	uint32_t addr = sys_get_le32(&cmd[4]);
	struct zelos_xcp_daq_list *d;
	struct zelos_xcp_odt *o;
	struct zelos_xcp_odt_entry *e;
	const uint8_t *src;
	uint32_t used = 0;

	/* The pointer is only valid until the allocation it points into changes. */
	if (xcp->ptr_daq >= xcp->daq_count) {
		err(xcp, ERR_SEQUENCE);
		return;
	}
	d = &xcp->daq[xcp->ptr_daq];
	if (xcp->ptr_odt >= d->odt_count) {
		err(xcp, ERR_SEQUENCE);
		return;
	}
	o = &xcp->odt[d->first_odt + xcp->ptr_odt];
	if (xcp->ptr_entry >= o->entry_count) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}
	/* Bit-wise entries (bit offset != 0xFF) are not supported. */
	if (cmd[1] != 0xFF || cmd[3] != 0 || size == 0 || size > ODT_PAYLOAD) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	src = resolve(xcp, addr, size, false);
	if (src == NULL) {
		err(xcp, ERR_ACCESS_DENIED);
		return;
	}

	e = &xcp->entry[o->first_entry + xcp->ptr_entry];
	for (uint8_t i = 0; i < o->entry_count; i++) {
		const struct zelos_xcp_odt_entry *other = &xcp->entry[o->first_entry + i];

		used += other == e ? size : other->size;
	}
	if (used > odt_room(xcp->ptr_odt, d->timestamp)) {
		err(xcp, ERR_DAQ_CONFIG);
		return;
	}

	e->ptr = src;
	e->size = size;
	xcp->ptr_entry++;
	(void)res(xcp, 1);
}

static void set_daq_list_mode(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint16_t channel = sys_get_le16(&cmd[4]);
	bool timestamp;

	if (d == NULL || channel >= xcp->config->event_count || cmd[6] == 0) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}
	if (cmd[1] & DAQ_MODE_UNSUPPORTED) {
		err(xcp, ERR_MODE_NOT_VALID);
		return;
	}
	/* Entries usually come first: a timestamp must still fit beside ODT 0's. */
	timestamp = (cmd[1] & DAQ_MODE_TIMESTAMP) != 0;
	if (d->odt_count != 0 && odt_used(xcp, &xcp->odt[d->first_odt]) > odt_room(0, timestamp)) {
		err(xcp, ERR_DAQ_CONFIG);
		return;
	}

	d->channel = (uint8_t)channel;
	d->prescaler = cmd[6];
	d->countdown = cmd[6];
	d->timestamp = timestamp;
	(void)res(xcp, 1);
}

static void get_daq_list_mode(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	const struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint8_t *r;

	if (d == NULL) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	r = res(xcp, 8);
	if (r != NULL) {
		r[1] = (d->selected ? DAQ_MODE_SELECTED : 0) | (d->timestamp ? DAQ_MODE_TIMESTAMP : 0) |
		       (d->running ? DAQ_MODE_RUNNING : 0);
		sys_put_le16(d->channel, &r[4]);
		r[6] = d->prescaler;
	}
}

static void start_stop_daq_list(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint8_t *r;

	if (d == NULL) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}
	if (cmd[1] > 2) {
		err(xcp, ERR_MODE_NOT_VALID);
		return;
	}
	if (cmd[1] != 0 && d->odt_count == 0) {
		err(xcp, ERR_DAQ_CONFIG);
		return;
	}

	switch (cmd[1]) {
	case 0:
		d->running = false;
		break;
	case 1:
		d->countdown = d->prescaler;
		d->running = true;
		break;
	default:
		d->selected = true;
		break;
	}

	r = res(xcp, 2);
	if (r != NULL) {
		r[1] = d->first_odt;
	}
}

static void start_stop_synch(struct zelos_xcp_core *xcp, uint8_t mode)
{
	if (mode > 2) {
		err(xcp, ERR_MODE_NOT_VALID);
		return;
	}

	for (uint8_t i = 0; i < xcp->daq_count; i++) {
		struct zelos_xcp_daq_list *d = &xcp->daq[i];

		/* Mode 0 stops every list; 1 and 2 start or stop the selected ones. */
		if (mode == 0) {
			d->running = false;
		} else if (d->selected) {
			d->running = mode == 1;
			d->countdown = d->prescaler;
		}
		d->selected = false;
	}
	(void)res(xcp, 1);
}

static void get_daq_event_info(struct zelos_xcp_core *xcp, uint16_t channel)
{
	const struct zelos_xcp_event_channel *ev;
	uint8_t *r;

	if (channel >= xcp->config->event_count) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}

	ev = &xcp->config->events[channel];
	r = res(xcp, 7);
	if (r != NULL) {
		r[1] = EVENT_PROPERTIES;
		r[2] = CONFIG_ZELOS_XCP_DAQ_LISTS;
		r[3] = (uint8_t)strlen(ev->name);
		r[4] = ev->period_ms;
		r[5] = TIME_UNIT_1MS;
		r[6] = 0; /* priority */
	}
	/* The master uploads the name from here. */
	xcp->mta = xcp_addr(ev->name);
}

/* Minimum CRO length per command, so no handler reads past the frame. */
static uint8_t min_len(uint8_t cmd)
{
	switch (cmd) {
	case CMD_SET_MTA:
	case CMD_SHORT_UPLOAD:
	case CMD_WRITE_DAQ:
		return 8;
	case CMD_SET_DAQ_LIST_MODE:
		return 7;
	case CMD_SET_DAQ_PTR:
	case CMD_ALLOC_ODT_ENTRY:
		return 6;
	case CMD_ALLOC_ODT:
		return 5;
	case CMD_START_STOP_DAQ_LIST:
	case CMD_ALLOC_DAQ:
	case CMD_GET_DAQ_EVENT_INFO:
	case CMD_GET_DAQ_LIST_MODE:
		return 4;
	case CMD_GET_ID:
	case CMD_UPLOAD:
	case CMD_DOWNLOAD:
	case CMD_START_STOP_SYNCH:
		return 2;
	default:
		return 1;
	}
}

/* Commands that change the DAQ configuration, refused while any list runs. */
static bool changes_daq(uint8_t cmd)
{
	switch (cmd) {
	case CMD_ALLOC_DAQ:
	case CMD_ALLOC_ODT:
	case CMD_ALLOC_ODT_ENTRY:
	case CMD_WRITE_DAQ:
	case CMD_SET_DAQ_LIST_MODE:
		return true;
	default:
		return false;
	}
}

void zelos_xcp_core_init(struct zelos_xcp_core *xcp, const struct zelos_xcp_config *config)
{
	memset(xcp, 0, sizeof(*xcp));
	xcp->config = config;
}

void zelos_xcp_core_on_frame(struct zelos_xcp_core *xcp, const uint8_t *cmd, uint8_t len,
			     uint32_t now)
{
	uint8_t *r;

	if (len == 0) {
		return;
	}

	/* A disconnected slave answers CONNECT and nothing else. */
	if (!xcp->connected && cmd[0] != CMD_CONNECT) {
		return;
	}

	if (len < min_len(cmd[0])) {
		err(xcp, ERR_CMD_SYNTAX);
		return;
	}

	if (changes_daq(cmd[0]) && daq_running(xcp)) {
		err(xcp, ERR_DAQ_ACTIVE);
		return;
	}

	switch (cmd[0]) {
	case CMD_CONNECT:
		connect(xcp);
		break;
	case CMD_DISCONNECT:
		daq_stop_all(xcp);
		xcp->connected = false;
		(void)res(xcp, 1);
		break;
	case CMD_GET_STATUS:
		r = res(xcp, 6);
		if (r != NULL) {
			r[1] = daq_running(xcp) ? SESSION_DAQ_RUNNING : 0;
		}
		break;
	case CMD_SYNCH:
		/* SYNCH is answered with an error by definition. */
		err(xcp, ERR_CMD_SYNCH);
		break;
	case CMD_GET_COMM_MODE_INFO:
		r = res(xcp, 8);
		if (r != NULL) {
			r[7] = 0x10; /* driver version 1.0 */
		}
		break;
	case CMD_GET_ID:
		get_id(xcp, cmd[1]);
		break;
	case CMD_SET_MTA:
		if (cmd[3] != 0) {
			err(xcp, ERR_OUT_OF_RANGE);
			break;
		}
		xcp->mta = sys_get_le32(&cmd[4]);
		(void)res(xcp, 1);
		break;
	case CMD_UPLOAD:
		upload(xcp, cmd[1]);
		break;
	case CMD_SHORT_UPLOAD:
		if (cmd[3] != 0) {
			err(xcp, ERR_OUT_OF_RANGE);
			break;
		}
		xcp->mta = sys_get_le32(&cmd[4]);
		upload(xcp, cmd[1]);
		break;
	case CMD_DOWNLOAD:
		if (cmd[1] == 0 || cmd[1] > len - 2) {
			err(xcp, ERR_OUT_OF_RANGE);
			break;
		}
		download(xcp, cmd[1], &cmd[2]);
		break;
	case CMD_FREE_DAQ:
		free_daq(xcp);
		break;
	case CMD_ALLOC_DAQ:
		alloc_daq(xcp, sys_get_le16(&cmd[2]));
		break;
	case CMD_ALLOC_ODT:
		alloc_odt(xcp, cmd);
		break;
	case CMD_ALLOC_ODT_ENTRY:
		alloc_odt_entry(xcp, cmd);
		break;
	case CMD_SET_DAQ_PTR:
		set_daq_ptr(xcp, cmd);
		break;
	case CMD_WRITE_DAQ:
		write_daq(xcp, cmd);
		break;
	case CMD_SET_DAQ_LIST_MODE:
		set_daq_list_mode(xcp, cmd);
		break;
	case CMD_START_STOP_DAQ_LIST:
		start_stop_daq_list(xcp, cmd);
		break;
	case CMD_START_STOP_SYNCH:
		start_stop_synch(xcp, cmd[1]);
		break;
	case CMD_GET_DAQ_LIST_MODE:
		get_daq_list_mode(xcp, cmd);
		break;
	case CMD_GET_DAQ_CLOCK:
		/* Legacy format: the DAQ timestamp clock, now. */
		r = res(xcp, 8);
		if (r != NULL) {
			sys_put_le32(now, &r[4]);
		}
		break;
	case CMD_GET_DAQ_PROCESSOR_INFO:
		r = res(xcp, 8);
		if (r != NULL) {
			r[1] = DAQ_PROPERTIES;
			r[2] = CONFIG_ZELOS_XCP_DAQ_LISTS;
			r[4] = (uint8_t)xcp->config->event_count;
			r[7] = DAQ_KEY_BYTE;
		}
		break;
	case CMD_GET_DAQ_RESOLUTION_INFO:
		r = res(xcp, 8);
		if (r != NULL) {
			r[1] = 1; /* granularity */
			r[2] = ODT_PAYLOAD;
			r[3] = 1;
			r[5] = TIMESTAMP_MODE;
			sys_put_le16(1, &r[6]); /* ticks per timestamp step */
		}
		break;
	case CMD_GET_DAQ_EVENT_INFO:
		get_daq_event_info(xcp, sys_get_le16(&cmd[2]));
		break;
	default:
		err(xcp, ERR_CMD_UNKNOWN);
		break;
	}
}

void zelos_xcp_core_event(struct zelos_xcp_core *xcp, uint8_t channel, uint32_t now)
{
	for (uint8_t i = 0; i < xcp->daq_count; i++) {
		struct zelos_xcp_daq_list *d = &xcp->daq[i];

		if (!d->running || d->channel != channel || --d->countdown != 0) {
			continue;
		}
		d->countdown = d->prescaler;

		/*
		 * A sample is all of a list's ODTs or none, so overload drops whole
		 * samples; EV_DAQ_OVERLOAD reports it. One slot stays free for a
		 * command response.
		 */
		if (ZELOS_XCP_TX_QUEUE - xcp->tx_len - 1 < d->odt_count) {
			xcp->overload = true;
			continue;
		}

		for (uint8_t o = 0; o < d->odt_count; o++) {
			const struct zelos_xcp_odt *odt = &xcp->odt[d->first_odt + o];
			struct zelos_xcp_frame *f = tx_push(xcp);

			f->data[0] = d->first_odt + o;
			f->len = 1;
			if (o == 0 && d->timestamp) {
				sys_put_le32(now, &f->data[1]);
				f->len += TIMESTAMP_SIZE;
			}
			for (uint8_t e = 0; e < odt->entry_count; e++) {
				const struct zelos_xcp_odt_entry *entry =
					&xcp->entry[odt->first_entry + e];

				/* Allocated but never written: contributes nothing. */
				if (entry->ptr != NULL) {
					memcpy(&f->data[f->len], entry->ptr, entry->size);
					f->len += entry->size;
				}
			}
		}
	}
}

bool zelos_xcp_core_poll(struct zelos_xcp_core *xcp, struct zelos_xcp_frame *out)
{
	if (xcp->tx_len == 0) {
		/* After the samples already queued, so it never displaces a response. */
		if (!xcp->overload) {
			return false;
		}
		xcp->overload = false;
		*out = (struct zelos_xcp_frame){.len = 2, .data = {PID_EV, EV_DAQ_OVERLOAD}};
		return true;
	}

	*out = xcp->tx[xcp->tx_head];
	xcp->tx_head = (xcp->tx_head + 1) % ZELOS_XCP_TX_QUEUE;
	xcp->tx_len--;

	return true;
}
