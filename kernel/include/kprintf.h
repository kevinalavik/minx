/* minx - kernel formatting.
 *
 * kprintf() is the only way the kernel is allowed to talk to the user.  It
 * formats into a small stack buffer and hands the result to the console, which
 * mirrors it to both the framebuffer and COM1.
 */
#ifndef MINX_KPRINTF_H
#define MINX_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
void ksprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Print without a trailing newline. */
void kputs(const char *s);

/* Progress-style output used while probing hardware. */
#define kinfo(fmt, ...)  kprintf("[ ok ] " fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)  kprintf("[warn] " fmt, ##__VA_ARGS__)
#define kerr(fmt, ...)   kprintf("[fail] " fmt, ##__VA_ARGS__)

#endif /* MINX_KPRINTF_H */