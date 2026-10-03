/* minx - power control. */
#ifndef MINX_POWER_H
#define MINX_POWER_H

#include <stdbool.h>

/* Try every mechanism available, in order of how well it is supported. */
void poweroff(void) __attribute__((noreturn));
void reboot(void) __attribute__((noreturn));

/* Individual mechanisms, exposed because they are also how the self test proves
 * the plumbing exists. */
void poweroff_acpi(void);
void poweroff_debug_exit(int code);
void reboot_keyboard_controller(void);
void reboot_qemu(void);
void reboot_acpi(void);

#endif /* MINX_POWER_H */