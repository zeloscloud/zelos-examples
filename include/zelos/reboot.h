/*
 * The one orderly MCU reboot.
 *
 * Every protocol that reboots the MCU on request (UDS ECUReset, over CAN or
 * DoIP) calls zelos_reboot(), so state that must survive a reboot is saved, and
 * peers told, on every path, not only the one that knew about it. UDS registers
 * its DTC memory sealing; DoIP closes its testers' connections; SOME/IP stops
 * offering its service. CANopen's NMT reset-node does not reboot; see canopen.h.
 */

#ifndef ZELOS_REBOOT_H_
#define ZELOS_REBOOT_H_

#include <zephyr/toolchain.h>

typedef void (*zelos_reboot_hook_t)(void);

#define ZELOS_REBOOT_HOOKS_MAX 4

/**
 * Run fn before every zelos_reboot(), the last registered first.
 *
 * @param fn Runs in the rebooting thread, whichever that is; may wait briefly
 *           (flash writes already queued, a message on the wire). A hook that
 *           holds a lock into the reboot (UDS's) is registered first, so runs last.
 */
void zelos_reboot_on(zelos_reboot_hook_t fn);

/**
 * Run the hooks, then reboot the MCU.
 *
 * @param type SYS_REBOOT_COLD or SYS_REBOOT_WARM, as sys_reboot() takes it.
 */
FUNC_NORETURN void zelos_reboot(int type);

#endif /* ZELOS_REBOOT_H_ */
