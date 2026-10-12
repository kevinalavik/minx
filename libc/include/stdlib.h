/* minx - <stdlib.h>
 *
 * Allocation, conversion, the environment, and the three ways a program ends.
 */
#ifndef MINX_STDLIB_H
#define MINX_STDLIB_H

#include <sys/sysdep.h>
#include <time.h>

/* --- allocation -------------------------------------------------------------- */

/* `malloc()` grows the heap with `brk()`, and `free()` gives a block back to its own free
 * list rather than to the kernel.
 *
 * A program that frees and then allocates the same size gets the same pointer back, which
 * is the property `free()` is *required* to have if it is going to do anything at all: a
 * shell that allocates a token for every word and frees it must not grow without bound.
 * `realloc()` therefore usually ends up being a `free()` followed by a `malloc()` and a
 * copy, and that is worth saying plainly rather than pretending there is a cleverer plan.
 *
 * Blocks carry an eight-byte header holding their size, so `free()` knows how much to put
 * back on the list without the caller having to say.  The cost is that a `malloc()` result
 * is eight-byte aligned and nothing is: `free()` has to round the pointer down, and a
 * caller that stores the real start somewhere else is a program whose `free()` frees the
 * wrong address. */
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

/* Not a POSIX call, and useful enough to be worth having: how much of the heap is in use
 * and how much the kernel has actually given this process.  A program that is about to
 * allocate something large wants to know the second number, and `sbrk(0)` is the way every
 * other libc spells it. */
void *sbrk(intptr_t increment);

/* --- program termination ----------------------------------------------------- */

/* `exit()` runs the atexit handlers, flushes the streams, and calls `_exit()`.  That order
 * matters: a handler that prints something has to be able to, and one that frees something
 * has to happen before the address space goes. */
void exit(int status);
void _Exit(int status);
void _exit(int status);
void abort(void);

/* The handlers are stored in a static array with room for a fixed number, which is a
 * deliberate limit rather than an oversight: the table cannot fail to allocate, so there
 * is no path on which registering a handler has to return an error. */
#define ATEXIT_MAX 32
int atexit(void (*fn)(void));

/* --- environment ------------------------------------------------------------- */

/* A static array of pointers into one static buffer.  A program can put a name in its own
 * string literal and `putenv()` it, which is what every shell does and what makes the
 * "the environment belongs to the process" story true: the strings are the program's, and
 * `putenv` does not copy them. */
char **environ;

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *entry);

/* --- conversion -------------------------------------------------------------- */

long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long   strtoll(const char *s, char **end, int base);
int    atoi(const char *s);
long   atol(const char *s);
double atof(const char *s);
double strtod(const char *s, char **end);

/* The `l` suffixes exist and are the ones to use.  `atoi` has no error reporting: a string
 * that is not a number becomes 0, which is also what a successful parse of "0" gives, and a
 * program that cannot tell the two apart has a bug it cannot find. */
/* --- misc -------------------------------------------------------------------- */

int   abs(int n);
long  labs(long n);

/* `qsort()` and `bsearch()` are here because a shell sorts a directory listing with them
 * and because leaving them out of a libc that claims to be ISO C is a hole rather than a
 * decision.  `qsort()` is a merge sort rather than quicksort: quicksort is faster on
 * random input and pathological on sorted input, and the input a program hands `qsort()` is
 * usually *already* ordered -- a directory listing, a path table, an argument list. */
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

/* The random numbers are not random.  There is no entropy source in the kernel, and a
 * generator seeded from `clock()` would look random enough to be used and would repeat
 * itself across two boots on the same machine -- which is a worse failure than one that is
 * visibly not random.  This one is seeded from the pid, the address of a stack object and
 * the clock, so two processes started in the same millisecond do not produce the same
 * sequence, and a program that needs real randomness has to be told so. */
int   rand(void);
void  srand(unsigned int seed);
long  random(void);
void  srandom(unsigned int seed);

/* `system()` runs a command through `/bin/sh`.  There is a shell in phase 9; until there is
 * one this returns -1 with ENOENT, which is the truth rather than a shell that silently
 * does nothing. */
int system(const char *command);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

#endif /* MINX_STDLIB_H */