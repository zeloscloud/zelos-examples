/*
 * CANopen node runtime on CANopenNode.
 *
 * The node owns its object dictionary (od/CO_OD.c, od/CO_OD.h, generated from
 * its EDS) and its business logic; this runs the stack around them: NMT,
 * heartbeat, SDO server, EMCY and PDOs as the dictionary configures them.
 */

#ifndef ZELOS_CANOPEN_H_
#define ZELOS_CANOPEN_H_

/**
 * Run the CANopen stack on the calling thread.
 *
 * tick runs every millisecond or so with the object dictionary locked: read
 * what the master wrote (RPDO, SDO) from it, and write what the node reports.
 * PDOs are sent and received by the stack's own thread.
 *
 * Call zelos_can_init() first: the stack shares the controller lib/can owns.
 *
 * Both NMT resets restart the stack in place, without touching the controller
 * or the MCU, and announce it with a boot-up message. Reset-communication
 * restores the communication area (0x1000-0x1FFF) to its values at this call;
 * reset-node restores the whole dictionary. Node state that a reset-node must
 * clear therefore lives in the dictionary.
 *
 * @param tick The node's business logic.
 * @return Only on failure to start: negative errno.
 */
int zelos_canopen_run(void (*tick)(void));

#endif /* ZELOS_CANOPEN_H_ */
