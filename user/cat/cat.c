/* `cat` -- copy standard input, or named files, to standard output.
 *
 * One of the six programs `shtest`'s script needs.  It appears there three times, and each
 * time for a different reason: to put a file's contents on the console, to be the *other
 * end* of a pipeline (`true | cat` is the check that a pipeline carries bytes and not just
 * exit statuses), and to read a file back inside a command substitution
 * (`echo "file=$(cat /tmp/x)"`).
 *
 * The pipeline use is the one that decides how it reads.  A whole-file `read()` into memory
 * would be simpler, and it would be wrong here: `a | cat | cat` must not deadlock, and the
 * version that reads a file's length first and then exactly that much is the version that
 * can be left blocked on a pipe that will never be that long.  So it reads a buffer at a
 * time and writes each one, which is the only shape that works for both.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* `minx_main()`, not `main()`.
 *
 * `user/common/start.S` calls `minx_main` and the libc's `exit.S` provides that entry for
 * every program that links the libc -- the entry stub and the C runtime have to agree on
 * one name, and every program in this tree that uses stdio has used this one.  A plain
 * `main()` here links to nothing: the error is "undefined reference to `minx_main'", which
 * says nothing about the cause.
 */
int minx_main(unsigned long argc, char **argv);

/* One read/write round.  Short for a file and short for a pipe both mean "that is what was
 * available", which is why the loop does not care which it is reading. */
#define CHUNK 4096

static int copy_stream(FILE *in) {
    char buf[CHUNK];

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);

        if (n > 0 && fwrite(buf, 1, n, stdout) != n)
            return 1;

        if (n < sizeof(buf)) {
            /* Short read: either the end, or an error.  `ferror()` tells them apart, and
             * it has to be asked -- a read that failed has returned a *count*, and the count
             * alone looks exactly like an end of file. */
            if (ferror(in)) {
                fprintf(stderr, "cat: read: %s\n", strerror(errno));
                return 1;
            }

            return 0;
        }
    }
}

int minx_main(unsigned long argc, char **argv) {
    /* `argc` is `unsigned long` because that is what the entry stub passes, and every loop
     * index below is an `int`.  They are the same value on this ABI; the cast says so once
     * rather than at every comparison, and says it because a `-Wsign-compare` that has been
     * silenced in the build is a real class of bug papered over rather than answered. */
    int  nargs = (int)argc;

    int status = 0;
    int i;

    /* No arguments, or a bare `-`: standard input to standard output.  That is the whole of
     * what a pipeline needs, and it is why `cat` with no arguments must not read from
     * anything else -- a script that writes into `cat` by accident has to block, because a
     * `cat` that read from a file or exited would swallow the write. */
    if (nargs == 1 || (nargs == 2 && strcmp(argv[1], "-") == 0))
        return copy_stream(stdin);

    for (i = 1; i < nargs; i++) {
        FILE *in;

        if (strcmp(argv[i], "-") == 0) {
            status |= copy_stream(stdin);
            continue;
        }

        in = fopen(argv[i], "r");

        if (in == NULL) {
            /* Reported and *skipped*, not fatal: `cat a b` where `a` does not exist still
             * prints `b`, and the status is non-zero at the end.  That is what every `cat`
             * does and what a script putting `cat` in a pipeline depends on. */
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (copy_stream(in) != 0)
            status = 1;

        fclose(in);
    }

    if (fflush(stdout) != 0 || ferror(stdout))
        status = 1;

    return status;
}