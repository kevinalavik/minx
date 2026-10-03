/* minx - COM1 serial driver (16550 compatible).
 *
 * COM1 is the kernel's mirror console and the channel the automated test
 * harness reads, so it is initialised as the very first thing after the
 * framebuffer.
 */
#ifndef MINX_SERIAL_H
#define MINX_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

#define COM1 0x3f8

/* Divisor for a given baud rate with the 1.8432 MHz UART clock. */
#define SERIAL_BAUD_DIVISOR(baud) (115200u / (baud))

int  serial_init(void);
void serial_putc(char c);
void serial_write(const char *data, size_t len);
void serial_puts(const char *s);
bool serial_has_input(void);
int  serial_getc(void);          /* blocking */
int  serial_getc_nonblock(void); /* -1 when nothing pending */
bool serial_initialized(void);

#endif /* MINX_SERIAL_H */