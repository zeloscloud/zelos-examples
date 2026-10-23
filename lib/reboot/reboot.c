#include <zelos/reboot.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

static zelos_reboot_hook_t hooks[ZELOS_REBOOT_HOOKS_MAX];
static size_t count;

void zelos_reboot_on(zelos_reboot_hook_t fn)
{
	__ASSERT(count < ARRAY_SIZE(hooks), "ZELOS_REBOOT_HOOKS_MAX hooks");
	if (count < ARRAY_SIZE(hooks)) {
		hooks[count++] = fn;
	}
}

FUNC_NORETURN void zelos_reboot(int type)
{
	/* Last registered first: UDS, the first, seals its memory last. */
	for (size_t i = count; i > 0; i--) {
		hooks[i - 1]();
	}
	sys_reboot(type);
}
