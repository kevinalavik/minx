/* minx - the two printing routines every user program needs.
 *
 * In user/common/ rather than in each program, because a program's own text is what the
 * loader is under test with: hello grew past 4 KiB of text and that is what exposed the
 * loader's per-page copy bug.  The less text a test program carries, the less of the test
 * is about the loader and the more of it is about what it is testing.
 */
#ifndef MINX_PRINT_H
#define MINX_PRINT_H

#include <stddef.h>

/* Write `s` to descriptor 1.  Returns the number of characters written, or a negative
 * errno. */
long minx_put(const char *s);

/* Write an unsigned number, in decimal, with nothing around it. */
long minx_put_num(unsigned long v);

/* Write a signed number, with a leading '-' for negatives. */
long minx_put_signed(long v);

/* Write `s` to descriptor `fd`. */
long minx_put_fd(int fd, const char *s);

/* How many characters `put` below has written.  A test program asserts on this rather than
 * capturing output, because there is nowhere in this kernel yet to capture it. */
unsigned long minx_put_count(void);

/* A fixed one-line report: "<name>: ok" or "<name>: <what went wrong>".  Every test program
 * ends with exactly one of these, so the kernel's self-test log has one line per program
 * to grep rather than a paragraph. */
long minx_report(const char *name, int ok, const char *detail);

#endif /* MINX_PRINT_H */