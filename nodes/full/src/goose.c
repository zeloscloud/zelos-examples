/*
 * The image's GOOSE (lib/goose): IED ZELOSFULL, LD LLN0, confRev 1, no VLAN.
 *
 *   gcb_pdu, APPID 0x0001 to 01:0C:CD:01:00:01, data set ds_pdu, goID
 *   zelos-pdu, on change: the PDU's channels 1-8 powered (Dbpos, on 10 closed,
 *   off 01 open), their trip latches 1-8 (BOOLEAN), then overcurrent EMCYs
 *   since boot (INT32).
 *   gcb_dcdc, APPID 0x0002 to 01:0C:CD:01:00:02, data set ds_dcdc, goID
 *   zelos-dcdc, every 100 ms: the DC-DC's output V, output A, input A and
 *   temperature degC (FLOAT32), then its 50 ms cycles since start (UNSIGNED).
 *   stNum counts changes of DcdcState (Idle, Converting, Derating, as its
 *   SOME/IP event and AUTOSAR PDUs have it), not of the values.
 *
 * The user button (sw0) toggles PDU channel 1's commanded state, 50 ms after
 * the last edge if still pressed.
 */

#include "goose.h"

#include <zelos/goose.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../../dcdc/src/dcdc_node.h"
#include "../../pdu/src/pdu_node.h"

LOG_MODULE_REGISTER(full_goose, LOG_LEVEL_INF);

#define CHANNELS 8
#define DEBOUNCE_MS 50

enum { IDLE, CONVERTING, DERATING };

static uint32_t read_pdu(struct zelos_goose_value *v)
{
	uint8_t powered;
	uint8_t tripped;
	int32_t emcy;

	pdu_status(&powered, &tripped, &emcy);
	for (int ch = 0; ch < CHANNELS; ch++) {
		v[ch] = (struct zelos_goose_value){
			.type = ZELOS_GOOSE_DBPOS,
			.dbpos = (powered & BIT(ch)) != 0U ? ZELOS_GOOSE_ON : ZELOS_GOOSE_OFF,
		};
		v[CHANNELS + ch] = (struct zelos_goose_value){
			.type = ZELOS_GOOSE_BOOLEAN,
			.boolean = (tripped & BIT(ch)) != 0U,
		};
	}
	v[2 * CHANNELS] = (struct zelos_goose_value){.type = ZELOS_GOOSE_INT32, .i32 = emcy};
	/* Every member; the EMCY count's low 16 bits are plenty between two reads. */
	return powered | tripped << 8 | (uint32_t)emcy << 16;
}

static uint32_t read_dcdc(struct zelos_goose_value *v)
{
	struct dcdc_sample s;

	dcdc_latest(&s);
	v[0] = (struct zelos_goose_value){.type = ZELOS_GOOSE_FLOAT32, .f32 = s.output_v};
	v[1] = (struct zelos_goose_value){.type = ZELOS_GOOSE_FLOAT32, .f32 = s.output_a};
	v[2] = (struct zelos_goose_value){.type = ZELOS_GOOSE_FLOAT32, .f32 = s.input_a};
	v[3] = (struct zelos_goose_value){.type = ZELOS_GOOSE_FLOAT32, .f32 = s.temperature_c};
	v[4] = (struct zelos_goose_value){.type = ZELOS_GOOSE_UINT32, .u32 = s.cycles};
	return s.derating ? DERATING : s.input_a > 0.0f ? CONVERTING : IDLE;
}

static const struct zelos_goose_cb cbs[] = {
	{
		.appid = 0x0001,
		.dst = {0x01, 0x0C, 0xCD, 0x01, 0x00, 0x01},
		.gocb_ref = "ZELOSFULL/LLN0$GO$gcb_pdu",
		.dat_set = "ZELOSFULL/LLN0$ds_pdu",
		.go_id = "zelos-pdu",
		.count = 2 * CHANNELS + 1,
		.read = read_pdu,
	},
	{
		.appid = 0x0002,
		.dst = {0x01, 0x0C, 0xCD, 0x01, 0x00, 0x02},
		.gocb_ref = "ZELOSFULL/LLN0$GO$gcb_dcdc",
		.dat_set = "ZELOSFULL/LLN0$ds_dcdc",
		.go_id = "zelos-dcdc",
		.period_ms = 100,
		.count = 5,
		.read = read_dcdc,
	},
};

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static void pressed(struct k_work *work)
{
	ARG_UNUSED(work);
	if (gpio_pin_get_dt(&button) == 1) {
		LOG_INF("button: channel 1 toggled");
		pdu_toggle_output(1);
	}
}

static K_WORK_DELAYABLE_DEFINE(debounce, pressed);

static void edge(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	(void)k_work_reschedule(&debounce, K_MSEC(DEBOUNCE_MS));
}

int full_goose_start(void)
{
	/* No button, no toggle: GOOSE goes on. */
	if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH) != 0) {
		LOG_WRN("user button unavailable");
	} else {
		gpio_init_callback(&button_cb, edge, BIT(button.pin));
		(void)gpio_add_callback(button.port, &button_cb);
	}
	return zelos_goose_start(cbs, ARRAY_SIZE(cbs));
}
