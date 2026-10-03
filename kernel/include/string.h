/* minx - freestanding string and memory helpers. */
#ifndef MINX_STRING_H
#define MINX_STRING_H

#include <stdbool.h>
#include <stddef.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void   memzero(void *dst, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);

/* Bounded string copy that always NUL-terminates.  Returns the length written
 * excluding the terminator, or -1 when the value did not fit. */
long strlcpy(char *dst, const char *src, size_t size);
long strlcat(char *dst, const char *src, size_t size);

/* Minimal base-conversion helpers used by printf and the userland tools. */
unsigned long long strtoull(const char *s, char **end, int base);
unsigned long long strtoull_pad(const char *s, char **end, int base,
                                bool *neg, int *digits_read);
int utoa(unsigned long long value, unsigned base, bool upper, char *out,
         size_t out_size);

#endif /* MINX_STRING_H */