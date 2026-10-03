#include "power.h"

#include "acpi.h"
#include "console.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "types.h"

/* QEMU's isa-debug-exit device: writing N makes it exit with (N << 1) | 1, which
 * is the only way an automated harness can tell a clean shutdown from a crash. */
#define QEMU_DEBUG_EXIT_PORT 0xf4

/* Bochs and friends also honour the "debug exit" port pair used by old cloud
 * images. */
#define BOGUS_DEBUG_EXIT_PORT 0x501

/* The 8042's reset line: pulse it low then release. */
#define PS2_RESET_CMD  0xfe

void poweroff_debug_exit(int code) {
    outb(QEMU_DEBUG_EXIT_PORT, (uint8_t)code);
    outb(BOGUS_DEBUG_EXIT_PORT, (uint8_t)(code == 0 ? 0x10 : 0x11));
}

void poweroff_acpi(void) {
    /* The S5 "power off" request through the PM1a control register.  Which
     * register is which varies with the machine, so both the 16-bit and the
     * 8-bit encodings are tried. */
    outw(0x604, 0x2000);
    io_wait();
    outb(0x604, 0x20);
    io_wait();
    outw(0xB004, 0x2000);
    io_wait();
    outb(0xB004, 0x20);
    io_wait();
}

void poweroff(void) {
    console_puts("\n[minx] powering off\n");

    poweroff_debug_exit(0);
    poweroff_acpi();

    panic("no mechanism for powering off is available on this machine");
}

void reboot_qemu(void) {
    /* QEMU's system reset port. */
    outw(0x604, 0x4000);
    io_wait();
    outb(0x64, 0xfe);   /* fall back to the keyboard controller below */
}

void reboot_keyboard_controller(void) {
    for (unsigned i = 0; i < 16; i++) {
        /* Pulse the 8042 reset line, waiting for the controller to be ready
         * between attempts. */
        while (inb(0x64) & 0x02)
            io_wait();
        outb(0x64, 0xfe);
    }

    /* Nothing above worked.  Falling through would return into whatever called
     * us, which for reboot() is the console's command loop. */
    kprintf("[warn] the 8042 reset line did not reboot the machine; "
            "powering off instead\\n");
    poweroff();
}

void reboot_acpi(void) {
    /* The S5 "soft off" value with the system reset bit, which most firmware
     * treats as a warm reset. */
    outw(0x604, 0x2000 | (0x80 << 8));
    io_wait();
    outw(0xB004, 0x2000 | (0x80 << 8));
    io_wait();
}

void reboot(void) {
    console_puts("\n[minx] rebooting\n");

    reboot_qemu();
    reboot_acpi();

    /* Nothing above worked; the 8042 reset line is the last resort, and it
     * falls back to a power off if even that does nothing. */
    reboot_keyboard_controller();
    khang();
}