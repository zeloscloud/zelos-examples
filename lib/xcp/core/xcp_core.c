/*
 * XCP slave protocol core. No I/O, no Zephyr calls: the glue feeds it CROs and
 * events and sends what it queues.
 */

#include <zelos/xcp.h>

#include <string.h>

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
	CMD_SHORT_DOWNLOAD = 0xED,
	CMD_SET_DAQ_PTR = 0xE2,
	CMD_WRITE_DAQ = 0xE1,
	CMD_SET_DAQ_LIST_MODE = 0xE0,
	CMD_START_STOP_DAQ_LIST = 0xDE,
	CMD_START_STOP_SYNCH = 0xDD,
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
};

enum {
	ERR_CMD_SYNCH = 0x00,
	ERR_CMD_UNKNOWN = 0x20,
	ERR_CMD_SYNTAX = 0x21,
	ERR_OUT_OF_RANGE = 0x22,
	ERR_ACCESS_DENIED = 0x24,
	ERR_MODE_NOT_VALID = 0x27,
	ERR_SEQUENCE = 0x29,
	ERR_DAQ_CONFIG = 0x2A,
	ERR_MEMORY_OVERFLOW = 0x30,
};

/* CONNECT: calibration and DAQ available; no STIM, no programming. */
#define RESOURCE_CAL_PAG 0x01
#define RESOURCE_DAQ 0x04
/* Intel byte order, byte granularity, GET_COMM_MODE_INFO available. */
#define COMM_MODE_BASIC 0x80
#define SESSION_DAQ_RUNNING 0x40
/* Dynamic configuration with prescaler; absolute ODT numbers as the PID. */
#define DAQ_PROPERTIES 0x03
#define DAQ_KEY_BYTE 0x00
#define EVENT_PROPERTY_DAQ 0x04
#define TIME_UNIT_1MS 6
/* DAQ list mode bits a master may set: none (no STIM, timestamps, PID_OFF). */
#define DAQ_MODE_UNSUPPORTED 0x3B

/* One PID byte per DTO; the rest is data. */
#define ODT_PAYLOAD (ZELOS_XCP_MAX_DTO - 1)
/* PIDs from 0xFC up are reserved for responses, errors, events and service. */
_Static_assert(CONFIG_ZELOS_XCP_ODTS <= 0xFC, "absolute ODT numbers must stay below 0xFC");

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

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

/*
 * The only way from an XCP address to a pointer. Reads may also reach the
 * strings the core itself hands out through the MTA (GET_ID, event names).
 */
static const uint8_t *resolve(const struct zelos_xcp_core *xcp, uint32_t addr, uint32_t len,
			      bool write)
{
	const struct zelos_xcp_config *cfg = xcp->config;

	for (size_t i = 0; i < cfg->region_count; i++) {
		const struct zelos_xcp_region *r = &cfg->regions[i];

		if ((r->writable || !write) && within(r->ptr, r->size, addr, len)) {
			return (const uint8_t *)r->ptr + (addr - xcp_addr(r->ptr));
		}
	}

	if (write) {
		return NULL;
	}

	if (within(cfg->id, strlen(cfg->id), addr, len)) {
		return (const uint8_t *)cfg->id + (addr - xcp_addr(cfg->id));
	}

	for (size_t i = 0; i < cfg->event_count; i++) {
		const char *name = cfg->events[i].name;

		if (within(name, strlen(name), addr, len)) {
			return (const uint8_t *)name + (addr - xcp_addr(name));
		}
	}

	return NULL;
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
	uint32_t len = type <= 1 ? strlen(xcp->config->id) : 0;
	uint8_t *r = res(xcp, 8);

	if (r != NULL) {
		/* Mode 0: the master uploads the text from the MTA. */
		put_u32(&r[4], len);
	}
	xcp->mta = xcp_addr(xcp->config->id);
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
		memcpy(&r[1], src, n);
		xcp->mta += n;
	}
}

static void download(struct zelos_xcp_core *xcp, uint8_t n, const uint8_t *data)
{
	uint8_t *dst = (uint8_t *)resolve(xcp, xcp->mta, n, true);

	if (dst == NULL) {
		err(xcp, ERR_ACCESS_DENIED);
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
	uint16_t i = get_u16(p);

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

	xcp->ptr_daq = get_u16(&cmd[2]);
	xcp->ptr_odt = cmd[4];
	xcp->ptr_entry = cmd[5];
	(void)res(xcp, 1);
}

static void write_daq(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	uint8_t size = cmd[2];
	uint32_t addr = get_u32(&cmd[4]);
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
	if (used > ODT_PAYLOAD) {
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
	uint16_t channel = get_u16(&cmd[4]);

	if (d == NULL || channel >= xcp->config->event_count || cmd[6] == 0) {
		err(xcp, ERR_OUT_OF_RANGE);
		return;
	}
	if (cmd[1] & DAQ_MODE_UNSUPPORTED) {
		err(xcp, ERR_MODE_NOT_VALID);
		return;
	}

	d->channel = (uint8_t)channel;
	d->prescaler = cmd[6];
	d->countdown = cmd[6];
	(void)res(xcp, 1);
}

static void start_stop_daq_list(struct zelos_xcp_core *xcp, const uint8_t *cmd)
{
	struct zelos_xcp_daq_list *d = daq_at(xcp, &cmd[2]);
	uint8_t *r;

	if (d == NULL || cmd[1] > 2) {
		err(xcp, ERR_OUT_OF_RANGE);
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
		err(xcp, ERR_OUT_OF_RANGE);
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
		r[1] = EVENT_PROPERTY_DAQ;
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
	case CMD_SHORT_DOWNLOAD:
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

void zelos_xcp_core_init(struct zelos_xcp_core *xcp, const struct zelos_xcp_config *config)
{
	memset(xcp, 0, sizeof(*xcp));
	xcp->config = config;
}

void zelos_xcp_core_on_frame(struct zelos_xcp_core *xcp, const uint8_t *cmd, uint8_t len)
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
		xcp->mta = get_u32(&cmd[4]);
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
		xcp->mta = get_u32(&cmd[4]);
		upload(xcp, cmd[1]);
		break;
	case CMD_DOWNLOAD:
		if (cmd[1] == 0 || cmd[1] > len - 2) {
			err(xcp, ERR_OUT_OF_RANGE);
			break;
		}
		download(xcp, cmd[1], &cmd[2]);
		break;
	case CMD_SHORT_DOWNLOAD:
		/* An 8-byte CTO leaves no room for data after the 8-byte header. */
		if (cmd[1] == 0 || cmd[1] > len - 8 || cmd[3] != 0) {
			err(xcp, ERR_OUT_OF_RANGE);
			break;
		}
		xcp->mta = get_u32(&cmd[4]);
		download(xcp, cmd[1], &cmd[8]);
		break;
	case CMD_FREE_DAQ:
		free_daq(xcp);
		break;
	case CMD_ALLOC_DAQ:
		alloc_daq(xcp, get_u16(&cmd[2]));
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
		}
		break;
	case CMD_GET_DAQ_EVENT_INFO:
		get_daq_event_info(xcp, get_u16(&cmd[2]));
		break;
	default:
		err(xcp, ERR_CMD_UNKNOWN);
		break;
	}
}

void zelos_xcp_core_event(struct zelos_xcp_core *xcp, uint8_t channel)
{
	for (uint8_t i = 0; i < xcp->daq_count; i++) {
		struct zelos_xcp_daq_list *d = &xcp->daq[i];

		if (!d->running || d->channel != channel || --d->countdown != 0) {
			continue;
		}
		d->countdown = d->prescaler;

		/*
		 * A sample is all of a list's ODTs or none, so overload drops whole
		 * samples. One slot stays free for a command response.
		 */
		if (ZELOS_XCP_TX_QUEUE - xcp->tx_len - 1 < d->odt_count) {
			continue;
		}

		for (uint8_t o = 0; o < d->odt_count; o++) {
			const struct zelos_xcp_odt *odt = &xcp->odt[d->first_odt + o];
			struct zelos_xcp_frame *f = tx_push(xcp);

			f->data[0] = d->first_odt + o;
			f->len = 1;
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
		return false;
	}

	*out = xcp->tx[xcp->tx_head];
	xcp->tx_head = (xcp->tx_head + 1) % ZELOS_XCP_TX_QUEUE;
	xcp->tx_len--;

	return true;
}
