/*
 * The one orderly MCU reboot.
 *
 * Every protocol that reboots the MCU on request (UDS ECUReset) calls
 * zelos_reboot(), so state that must survive a reboot is saved on every path,
 * not only the one that knew about it. UDS registers its DTC memory sealing as
 * the hook. CANopen's NMT reset-node does not reboot; see canopen.h.
 */

#ifndef ZELOS_REBOOT_H_
#define ZELOS_REBOOT_H_

#include <zephyr/toolchain.h>

typedef void (*zelos_reboot_hook_t)(void);

/**
 * Run fn before every zelos_reboot(). One hook per firmware: UDS's.
 *
 * @param fn Runs in the rebooting thread; must not block.
 */
void zelos_reboot_on(zelos_reboot_hook_t fn);

/**
 * Run the hook, then reboot the MCU.
 *
 * @param type SYS_REBOOT_COLD or SYS_REBOOT_WARM, as sys_reboot() takes it.
 */
FUNC_NORETURN void zelos_reboot(int type);

#endif /* ZELOS_REBOOT_H_ */
