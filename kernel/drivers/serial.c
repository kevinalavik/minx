#include "serial.h"

#include "io.h"
#include "string.h"

/* 16550 register offsets from COM1. */
#define REG_DATA         0
#define REG_IER          1
#define REG_FCR          2
#define REG_LCR          3
#define REG_MCR          4
#define REG_LSR          5
#define REG_MSR          6
#define REG_SCR          7

#define LSR_DATA_READY   0x01
#define LSR_THR_EMPTY    0x20
#define IER_RX_AVAILABLE 0x01

static bool initialized;

int serial_init(void) {
    outb(COM1 + REG_IER, 0x00);      /* no interrupts while we set up */
    outb(COM1 + REG_LCR, 0x80);      /* DLAB on */
    outb(COM1 + REG_DATA, (uint8_t)(SERIAL_BAUD_DIVISOR(115200) & 0xff));
    outb(COM1 + REG_IER, (uint8_t)((SERIAL_BAUD_DIVISOR(115200) >> 8) & 0xff));
    outb(COM1 + REG_LCR, 0x03);      /* DLAB off, 8N1 */
    outb(COM1 + REG_FCR, 0xc7);      /* enable+clear FIFOs, 14 byte trigger */
    outb(COM1 + REG_MCR, 0x0b);      /* DTR, RTS, OUT2 (IRQs) */

    initialized = true;
    return 0;
}

bool serial_initialized(void) {
    return initialized;
}

void serial_putc(char c) {
    /* Bring the UART up on first use.  Bring-up code (and the exception
     * handlers it can trip over) prints before serial_init() runs, and losing
     * that output is exactly the diagnostic that is needed most. */
    if (!initialized)
        serial_init();

    /* QEMU is fast enough that this never blocks in practice, but a real UART
     * on a loaded machine would drop bytes without the wait. */
    unsigned spins = 0;
    while ((inb(COM1 + REG_LSR) & LSR_THR_EMPTY) == 0) {
        if (++spins > 100000u)
            break;
    }
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void serial_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        serial_putc(data[i]);
}

void serial_puts(const char *s) {
    serial_write(s, strlen(s));
}

bool serial_has_input(void) {
    return initialized && (inb(COM1 + REG_LSR) & LSR_DATA_READY) != 0;
}

int serial_getc_nonblock(void) {
    if (!serial_has_input())
        return -1;
    return (int)(inb(COM1 + REG_DATA) & 0xff);
}

int serial_getc(void) {
    while (!serial_has_input())
        ;
    return serial_getc_nonblock();
}