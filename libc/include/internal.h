/* minx - what libc's own files share, and what a program must not include.
 *
 * The stdio internals live here rather than in <stdio.h> because `exit()` has to flush the
 * streams and the atexit handlers, and a *program* has no business calling either.  One
 * header for the two of them is less than a header each would be.
 */
#ifndef MINX_INTERNAL_H
#define MINX_INTERNAL_H

#include <stddef.h>

/* Run everything registered with `atexit()`, most recent first.  Called by `exit()` and
 * by nothing else -- `_exit()` must not, which is the entire difference between them. */
void __stdio_run_atexit(void);

/* Flush every open stream.  Also called by `exit()`, and directly by a program that is
 * about to `_exit()` and still wants its output out. */
void __stdio_flush_all(void);

/* The one exit path every program ends in, whatever it calls to get there. */
int  __stdio_fd_count(void);

#endif /* MINX_INTERNAL_H */
