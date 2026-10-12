/* minx - <string.h>
 *
 * ISO C's string functions, and the POSIX ones that come with them.  Every one is in libc
 * rather than a header because `strlen()` is the one function in a C program that the
 * compiler will not emit for itself when it is built `-ffreestanding`, and a libc without
 * it is a libc every program has to supply for itself.
 */
#ifndef MINX_STRING_H
#define MINX_STRING_H

#include <stddef.h>

/* --- ISO C ------------------------------------------------------------------ */

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delim);
/* POSIX, but in <string.h> on every Unix that has them without a <strings.h> split. */
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);

/* --- POSIX ------------------------------------------------------------------ */

char  *strtok_r(char *s, const char *delim, char **save);
void   bzero(void *s, size_t n);              /* the old name for memset(s, 0, n) */
int    bcmp(const void *a, const void *b, size_t n);
void   bcopy(const void *src, void *dst, size_t n);
int    ffs(int i);

/* `strcasecmp()` and `strncasecmp()` are in <strings.h> on every Unix, and a program that
 * includes <string.h> expecting them is only wrong on the strictest reading of POSIX.  Both
 * spellings are declared here. */
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

#endif /* MINX_STRING_H */