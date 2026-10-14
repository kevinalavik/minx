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
#include <stdint.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Formatting into a buffer.
 *
 * `ksnprintf()` returns the number of characters it actually stored -- never more than
 * `size - 1`, and never more than the buffer holds -- so the answer is safe to hand
 * straight to something that copies.  That is deliberately *not* C99's `snprintf`, which
 * returns the length it would have written; see the comment at the end of `kvsnprintf()`
 * in kernel/lib/kprintf.c for why the difference matters here.  A caller that wants to
 * know whether anything was cut off compares the answer with `size - 1`. */

/* Bring the print lock up.  Once, from kmain(), before anything can print from more than
 * one processor.  See the comment on kprintf() for why there is a lock at all. */
void kprintf_init(void);
void kvprintf(const char *fmt, va_list ap);
void ksprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Print without a trailing newline. */
void kputs(const char *s);

/* Nanoseconds since the kernel last printed anything, or 0 if it never has.
 *
 * The boot task uses this to notice that the machine has stopped: a hang is silent, the
 * log simply stops in the middle of a line, and the line before the silence says what the
 * machine was about to do rather than where it stopped.  With a number, "nothing has been
 * printed for twenty seconds" is something to act on. */
uint64_t kprintf_silence_ns(void);

/* Progress-style output used while probing hardware. */
#define kinfo(fmt, ...)  kprintf("[ ok ] " fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)  kprintf("[warn] " fmt, ##__VA_ARGS__)
#define kerr(fmt, ...)   kprintf("[fail] " fmt, ##__VA_ARGS__)

#endif /* MINX_KPRINTF_H */