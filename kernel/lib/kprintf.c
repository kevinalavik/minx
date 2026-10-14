#include "kprintf.h"
#include "console.h"
#include "spinlock.h"
#include "timer.h"
#include "spinlock.h"

#include "console.h"
#include "string.h"
#include "types.h"

/* A printf that supports %s %c %d %i %u %x %X %p %lu %llu %lld %% with the
 * usual length modifiers, which is everything the kernel needs. */

typedef struct {
    char  *buf;
    size_t size;
    size_t written;   /* bytes that would have been written */
    int    error;
} sink_t;

static void sink_putc(sink_t *s, char c) {
    if (s->buf != NULL && s->written + 1 < s->size)
        s->buf[s->written] = c;
    s->written++;
}

static void sink_puts(sink_t *s, const char *str) {
    while (*str)
        sink_putc(s, *str++);
}

static void sink_pad(sink_t *s, const char *str, int width, bool left,
                     char pad) {
    int len = (int)strlen(str);
    if (!left) {
        for (int i = len; i < width; i++)
            sink_putc(s, pad);
    }
    sink_puts(s, str);
    if (left) {
        for (int i = len; i < width; i++)
            sink_putc(s, ' ');
    }
}

static const char *digits_lower = "0123456789abcdef";
static const char *digits_upper = "0123456789ABCDEF";

static int format_unsigned(char *out, size_t out_size, unsigned long long v,
                           unsigned base, bool upper) {
    char tmp[70];
    size_t n = 0;
    if (base < 2 || base > 36)
        return -1;
    const char *digits = upper ? digits_upper : digits_lower;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0 && n < sizeof(tmp));

    if (n + 1 > out_size)
        return -1;
    size_t o = 0;
    while (n-- > 0)
        out[o++] = tmp[n];
    out[o] = '\0';
    return (int)o;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    sink_t s = { buf, size, 0, 0 };
    if (size > 0 && buf != NULL)
        buf[0] = '\0';

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            sink_putc(&s, *p);
            continue;
        }

        p++;
        bool left = false;
        bool zero = false;
        bool plus = false;
        bool space = false;
        bool alt = false;

        for (;;) {
            if (*p == '-') { left = true; p++; }
            else if (*p == '0') { zero = true; p++; }
            else if (*p == '+') { plus = true; p++; }
            else if (*p == ' ') { space = true; p++; }
            else if (*p == '#') { alt = true; p++; }
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            p++;
            if (width < 0) {
                left = true;
                width = -width;
            }
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    precision = precision * 10 + (*p++ - '0');
            }
            if (precision < 0)
                precision = -1;
        }

        /* Length modifiers.  The kernel only ever needs to widen to long long;
         * anything shorter is promoted on the way in. */
        int length = 0;   /* 0=int 1=long 2=long long */
        for (;;) {
            if (*p == 'l') {
                length = (length == 1) ? 2 : 1;
                p++;
            } else if (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't') {
                p++;
            } else if (*p == 'q') {
                length = 2;
                p++;
            } else {
                break;
            }
        }

        char numbuf[72];
        const char *sign = "";

        switch (*p) {
        case 'd':
        case 'i': {
            long long v;
            if (length == 2)
                v = va_arg(ap, long long);
            else if (length == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);

            unsigned long long mag;
            if (v < 0) {
                sign = "-";
                mag = (unsigned long long)(-(v + 1)) + 1ull;
            } else {
                mag = (unsigned long long)v;
                if (plus)
                    sign = "+";
                else if (space)
                    sign = " ";
            }
            int n = format_unsigned(numbuf, sizeof(numbuf), mag, 10, false);
            if (n < 0)
                break;

            int total = n + (int)strlen(sign);

            /* An explicit precision zero-pads the digits themselves. */
            if (precision > n) {
                sink_puts(&s, sign);
                for (int i = n; i < precision; i++)
                    sink_putc(&s, '0');
                sink_puts(&s, numbuf);
                break;
            }

            if (left) {
                sink_puts(&s, sign);
                sink_puts(&s, numbuf);
                for (int i = total; i < width; i++)
                    sink_putc(&s, ' ');
            } else if (zero) {
                /* Zeros go between the sign and the digits, so that "-42"
                 * pads to "-0042" rather than "-  42". */
                sink_puts(&s, sign);
                for (int i = total; i < width; i++)
                    sink_putc(&s, '0');
                sink_puts(&s, numbuf);
            } else {
                for (int i = total; i < width; i++)
                    sink_putc(&s, ' ');
                sink_puts(&s, sign);
                sink_puts(&s, numbuf);
            }
            break;
        }

        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v;
            unsigned base = 10;
            bool upper = false;

            if (*p == 'x' || *p == 'X') {
                base = 16;
                upper = (*p == 'X');
            }
            if (length == 2)
                v = va_arg(ap, unsigned long long);
            else if (length == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned);

            int n = format_unsigned(numbuf, sizeof(numbuf), v, base, upper);
            if (n < 0)
                break;

            if (alt && base == 16 && v != 0) {
                sink_puts(&s, upper ? "0X" : "0x");
                sink_pad(&s, numbuf, width > 2 ? width - 2 : 0, left,
                         zero ? '0' : ' ');
            } else {
                sink_pad(&s, numbuf, width, left, zero ? '0' : ' ');
            }
            break;
        }

        case 'p': {
            /* A pointer is always pointer-sized, so it must be read as one
             * regardless of the length modifiers the caller happened to write. */
            void *ptr = va_arg(ap, void *);
            unsigned long long v = (unsigned long long)(uintptr_t)ptr;

            if (v == 0) {
                sink_puts(&s, "(nil)");
                break;
            }
            if (width == 0)
                width = 16;
            int n = format_unsigned(numbuf, sizeof(numbuf), v, 16, false);
            if (n < 0)
                break;
            sink_puts(&s, "0x");
            sink_pad(&s, numbuf, width, left, zero ? '0' : ' ');
            break;
        }

        case 'c': {
            char c = (char)va_arg(ap, int);
            sink_pad(&s, (char[2]){ c, '\0' }, width, left, ' ');
            break;
        }

        case 's': {
            const char *str = va_arg(ap, const char *);
            if (str == NULL)
                str = "(null)";
            size_t len = strlen(str);
            if (precision >= 0 && (size_t)precision < len)
                len = (size_t)precision;

            int pad_count = width - (int)len;
            if (!left) {
                for (int i = 0; i < pad_count; i++)
                    sink_putc(&s, ' ');
            }
            for (size_t i = 0; i < len; i++)
                sink_putc(&s, str[i]);
            if (left) {
                for (int i = 0; i < pad_count; i++)
                    sink_putc(&s, ' ');
            }
            break;
        }

        case '%':
            sink_putc(&s, '%');
            break;

        case '\0':
            return (int)s.written;

        default:
            sink_putc(&s, '%');
            sink_putc(&s, *p);
            break;
        }
    }

    /* The NUL goes where it always goes, and the return value is the number of characters
     * that actually *fit*.
     *
     * C99's `snprintf` returns the length it would have written and the caller is expected
     * to compare it with the buffer's size.  That is a fine contract for a hosted libc with
     * a `strlen` on every call, and it is a trap in a kernel: `ksnprintf(seq, 32, ...)`
     * returning 45 makes `console_write(seq, n)` read thirteen bytes past the end of a
     * thirty-two byte array, which is exactly what happened -- the terminal printed the
     * escape sequence with its final `m` replaced by a NUL and six bytes of somebody
     * else's stack, the serial mirror stopped being text, and `make test` reported
     * "43 of 44" for a run in which the guest's own tally said 44 of 44.
     *
     * So the kernel's printf returns what it stored.  A caller that wants to know whether
     * something was truncated compares the answer with `size - 1`, which is the same
     * comparison C99 asks for, just with the number it has to do it with being the one
     * that is safe to use. */
    if (s.buf != NULL && s.size > 0)
        s.buf[s.written < s.size ? s.written : s.size - 1] = '\0';

    if (s.written >= s.size) {
        s.error = -1;
        return (int)(s.size > 0 ? s.size - 1 : 0);
    }

    return (int)s.written;
}

void ksprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kvprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return;
    console_write_locked(buf, strlen(buf));
}

/* One lock for the whole of a formatted write.
 *
 * The console is shared: the framebuffer, the serial mirror and the serial-only heartbeat
 * are all writing into the same buffer, from four processors, and a write is a *sequence*
 * of characters rather than a character.  Without a lock two of them interleave inside each
 * other, which is why the boot log has lines like "it: console is descriptor 3" -- the
 * middle of a word from one processor and the end of a line from another.
 *
 * That is cosmetic in the sense that nothing is wrong, and it is not cosmetic in the sense
 * that it makes the serial log useless as evidence: a message can be split in half and
 * neither half is findable by `grep`.  Every phase in this kernel was verified by reading
 * that log, and "the line is there somewhere" is not the same as "the log says it".
 *
 * IRQ-saving, because `kprintf` is called from interrupt paths -- an unexpected vector, a
 * fault report -- and a plain spinlock there would deadlock against a `kprintf` that was
 * interrupted. */
/* When the last thing printed was printed, so that a kernel that has stopped can say so.
 * Written by `kprintf()` only, and read by one processor in the boot task's idle loop. */
static volatile uint64_t last_print_ns;

void kprintf(const char *fmt, ...) {
    va_list ap;

    /* The console's own lock, not a second one: a print is a write, and a write that takes
     * a different lock from every other write is a write that can still interleave with
     * them. */
    console_lock();

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    last_print_ns = timer_uptime_ms() * 1000000ull;

    console_unlock();
}

/* How long the kernel has said nothing, in nanoseconds.
 *
 * This exists because a hang is otherwise silent.  A machine that has stopped prints
 * nothing, the log ends in the middle of whatever it was doing, and everything after that
 * is inference: the last line says what the program was *about* to do, not where it
 * stopped.  With a number behind it, "the last thing printed was twenty seconds ago" is a
 * fact the boot task can act on -- which is what `tasks_report()` behind the hang check in
 * `kernel.c` does. */
uint64_t kprintf_silence_ns(void) {
    uint64_t last = last_print_ns;
    uint64_t now  = timer_uptime_ms() * 1000000ull;

    return (last == 0 || now < last) ? 0 : now - last;
}

/* Bring the print lock up.  Called once, from kmain(), after the serial port and before
 * anything that will want to print from more than one processor.  The lock itself lives in
 * console.c, because that is what it guards; this is here so the order in kmain() reads as
 * "the console, then the console's lock, then everything else". */
void kprintf_init(void) {
    console_lock_init();
}

void kputs(const char *s) {
    console_puts(s);
}