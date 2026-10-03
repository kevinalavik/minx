#include "string.h"

#include "io.h"
#include "types.h"

void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    /* Align to 8 bytes first, then blast through qwords. */
    while (n && ((uintptr_t)p & 7u)) {
        *p++ = v;
        n--;
    }
    uint64_t wide = (uint64_t)v;
    wide |= wide << 8;
    wide |= wide << 16;
    wide |= wide << 32;
    while (n >= 8) {
        *(uint64_t *)p = wide;
        p += 8;
        n -= 8;
    }
    while (n--)
        *p++ = v;
    return dst;
}

void memzero(void *dst, size_t n) {
    memset(dst, 0, n);
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n >= 8 && !(((uintptr_t)d | (uintptr_t)s) & 7u)) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s)
        return memcpy(dst, src, n);

    /* Overlapping, destination above source: copy backwards. */
    d += n;
    s += n;
    while (n >= 8) {
        d -= 8;
        s -= 8;
        *(uint64_t *)d = *(const uint64_t *)s;
        n -= 8;
    }
    while (n--)
        *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == v)
            return (void *)(p + i);
    }
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t i = 0;
    while (i < max && s[i])
        i++;
    return i;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static inline char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

char *strchr(const char *s, int c) {
    char v = (char)c;
    for (;; s++) {
        if (*s == v)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char v = (char)c;
    const char *found = NULL;
    for (;; s++) {
        if (*s == v)
            found = s;
        if (*s == '\0')
            return (char *)found;
    }
}

char *strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return (char *)hay;
    for (; *hay; hay++) {
        if (strncmp(hay, needle, n) == 0)
            return (char *)hay;
    }
    return NULL;
}

long strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return (long)len;
}

long strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strnlen(dst, size);
    size_t slen = strlen(src);
    if (dlen == size)
        return (long)(size + slen);
    size_t space = size - dlen;
    size_t copy = slen < space - 1 ? slen : space - 1;
    memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = '\0';
    return (long)(dlen + slen);
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

int utoa(unsigned long long value, unsigned base, bool upper, char *out,
         size_t out_size) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (base < 2 || base > 36 || out_size == 0)
        return -1;

    char tmp[70];
    size_t len = 0;
    do {
        tmp[len++] = digits[value % base];
        value /= base;
    } while (value != 0 && len < sizeof(tmp));

    if (len + 1 > out_size)
        return -1;

    size_t o = 0;
    while (len-- > 0)
        out[o++] = tmp[len];
    out[o] = '\0';
    return (int)o;
}

unsigned long long strtoull_pad(const char *s, char **end, int base,
                                bool *neg, int *digits_read) {
    const char *p = s;
    unsigned long long value = 0;
    int digits = 0;
    bool negative = false;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            base = 2;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    for (;;) {
        int d = digit_value(*p);
        if (d < 0 || (unsigned)d >= (unsigned)base)
            break;
        value = value * (unsigned)base + (unsigned)d;
        p++;
        digits++;
    }

    if (neg)
        *neg = negative;
    if (digits_read)
        *digits_read = digits;
    if (end)
        *end = (char *)p;
    return value;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return strtoull_pad(s, end, base, NULL, NULL);
}