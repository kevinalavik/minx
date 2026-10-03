/* minx - panic path.
 *
 * A panic must never return and must never try to allocate: it prints a
 * register dump and a best-effort stack trace on both the graphical console
 * and COM1, then stops the machine with a distinctive QEMU debug-exit code so
 * the test harness can tell a crash from a clean shutdown.
 */
#ifndef MINX_PANIC_H
#define MINX_PANIC_H

#include <stdint.h>

struct gprs;

/* Print the message plus a register dump, then halt forever. */
void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Halt the CPU (and, when known, the machine) without printing anything. */
void khang(void) __attribute__((noreturn));

/* Write one character straight to COM1, bypassing every other layer.  Used by
 * the early boot markers to bisect a failure that happens before the console
 * exists.  They are compiled out unless MINX_BOOT_TRACE is defined, because
 * they would otherwise scribble over the first line of the banner. */
void kboot_marker(char c);

#ifdef MINX_BOOT_TRACE
#define boot_trace(c) kboot_marker((char)(c))
#else
#define boot_trace(c) ((void)0)
#endif

/* Clean shutdown.  exit_code 0 means success, anything else is a failure. */
void kshutdown(int exit_code) __attribute__((noreturn));

/* Print the current CPU's general purpose registers. */
void dump_registers(void);

/* Walk the current stack looking for return addresses inside the kernel. */
void dump_stack_trace(void);

#endif /* MINX_PANIC_H */