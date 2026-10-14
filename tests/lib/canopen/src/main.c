/*
 * lib/canopen on the loopback controller, the test as the master: both NMT
 * resets restart the stack in place, and frames that match two of its filters
 * reach the right one, once.
 */

#include <zelos/can.h>
#include <zelos/canopen.h>

#include <canopennode.h>
/* After the stack, which defines the types it uses. */
#include <CO_OD.h>

#include <zephyr/ztest.h>

#define NODE      CONFIG_ZELOS_CANOPEN_NODE_ID
#define HEARTBEAT (0x700 + NODE)
#define BOOTUP    0x00

#define RESET_NODE          0x81
#define RESET_COMMUNICATION 0x82

K_MSGQ_DEFINE(test_heartbeat, sizeof(struct can_frame), 8, 4);
static K_THREAD_STACK_DEFINE(stack, 4096);
static struct k_thread thread;

static K_SEM_DEFINE(emcy_sem, 0, 8);
static uint16_t emcy_ident;

static void tick(void)
{
}

static void run(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)zelos_canopen_run(tick);
}

static void expect_bootup(void)
{
	struct can_frame got;

	do {
		zassert_ok(k_msgq_get(&test_heartbeat, &got, K_SECONDS(2)), "no boot-up");
	} while (got.data[0] != BOOTUP);
}

static void nmt(uint8_t command)
{
	const uint8_t data[] = {command, NODE};

	k_msgq_purge(&test_heartbeat);
	zassert_ok(zelos_can_send(0x000, data, sizeof(data)));
	expect_bootup();
}

static void *setup(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

	zassert_ok(can_set_mode(dev, CAN_MODE_LOOPBACK));
	zassert_ok(zelos_can_init());
	zassert_true(zelos_can_subscribe(HEARTBEAT, &test_heartbeat) >= 0);
	k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, NULL, NULL, NULL,
			K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	expect_bootup();

	return NULL;
}

/* Each test starts from the node's defaults. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	nmt(RESET_NODE);
}

/* A reboot would end this process: reaching the asserts is the proof there was none. */
ZTEST(zelos_canopen, test_reset_node_restores_every_area)
{
	const uint16_t heartbeat_ms = OD_producerHeartbeatTime;

	CO_LOCK_OD();
	OD_writeOutput8Bit.output1To8 = 0x05;
	OD_producerHeartbeatTime = 100;
	CO_UNLOCK_OD();

	nmt(RESET_NODE);

	CO_LOCK_OD();
	zassert_equal(OD_writeOutput8Bit.output1To8, 0);
	zassert_equal(OD_producerHeartbeatTime, heartbeat_ms);
	CO_UNLOCK_OD();
}

ZTEST(zelos_canopen, test_reset_communication_keeps_the_application)
{
	const uint16_t heartbeat_ms = OD_producerHeartbeatTime;

	CO_LOCK_OD();
	OD_writeOutput8Bit.output1To8 = 0x05;
	OD_producerHeartbeatTime = 100;
	CO_UNLOCK_OD();

	nmt(RESET_COMMUNICATION);

	CO_LOCK_OD();
	zassert_equal(OD_writeOutput8Bit.output1To8, 0x05);
	zassert_equal(OD_producerHeartbeatTime, heartbeat_ms);
	CO_UNLOCK_OD();
}

static void on_emcy(const uint16_t ident, const uint16_t error_code, const uint8_t error_register,
		    const uint8_t error_bit, const uint32_t info_code)
{
	ARG_UNUSED(error_code);
	ARG_UNUSED(error_register);
	ARG_UNUSED(error_bit);
	ARG_UNUSED(info_code);

	emcy_ident = ident;
	k_sem_give(&emcy_sem);
}

/*
 * SYNC 0x080 matches the SYNC filter and the EMCY consumer's 0x080/0x780. The
 * loopback controller reports every matching filter, so this also shows each
 * frame is handled once.
 */
ZTEST(zelos_canopen, test_sync_and_emcy_each_reach_their_own)
{
	const uint8_t emcy[8] = {0x00, 0x10, 0x01};
	bool toggle;

	k_sem_reset(&emcy_sem);
	CO_LOCK_OD();
	CO_EM_initCallbackRx(CO->em, on_emcy);
	toggle = CO->SYNC->CANrxToggle;
	CO_UNLOCK_OD();

	zassert_ok(zelos_can_send(0x080, emcy, 0));
	zassert_ok(zelos_can_send(0x081, emcy, sizeof(emcy)));

	zassert_ok(k_sem_take(&emcy_sem, K_SECONDS(1)), "EMCY not received");
	zassert_equal(emcy_ident, 0x081);
	zassert_equal(k_sem_take(&emcy_sem, K_MSEC(50)), -EAGAIN, "SYNC reached EMCY");
	/* Toggled once per SYNC received: twice would put it back. */
	zassert_not_equal(CO->SYNC->CANrxToggle, toggle, "SYNC not received once");
}

ZTEST_SUITE(zelos_canopen, NULL, setup, before, NULL, NULL);
