#include "kprintf.h"

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
        case 'X':
        case 'p': {
            unsigned long long v;
            unsigned base = 10;
            bool upper = false;

            if (*p == 'x' || *p == 'X') {
                base = 16;
                upper = (*p == 'X');
            } else if (*p == 'p') {
                base = 16;
                if (width == 0)
                    width = 18;
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

    if (s.buf != NULL && s.size > 0)
        s.buf[s.written < s.size ? s.written : s.size - 1] = '\0';
    if (s.written >= s.size)
        s.error = -1;
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
    console_write(buf, strlen(buf));
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void kputs(const char *s) {
    console_puts(s);
}