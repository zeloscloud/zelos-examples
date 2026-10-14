#include <zelos/reboot.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

static zelos_reboot_hook_t hook;

void zelos_reboot_on(zelos_reboot_hook_t fn)
{
	__ASSERT(hook == NULL, "one reboot hook");
	hook = fn;
}

FUNC_NORETURN void zelos_reboot(int type)
{
	if (hook != NULL) {
		hook();
	}
	sys_reboot(type);
}
