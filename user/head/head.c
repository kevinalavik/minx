/* `head` -- the first lines of a file.
 *
 * One of the six programs `shtest`'s script needs, and it is the other end of the pipeline
 * that decides whether a pipeline carries bytes: `ls /tmp/shtest-* | head -n 1` has to
 * print the first name and stop, and the shell has to not wait for a program that never
 * writes again.
 *
 * Which means the *early exit* is the interesting part.  `head` reads one line more than it
 * needs in order to know it is finished, and then stops reading -- it must not read the rest
 * of a file, and it must not block on a pipe that will never be closed.  So the read loop
 * breaks as soon as the count is met, and the "read until `\n`" that finds the end of the
 * last line stops there too.
 *
 * The default is ten lines, as everywhere, and `-n N` overrides it in both spellings.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_LINES 10

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
int  nargs = (int)argc;    /* `argc` arrives as an `unsigned long` because that is what the entry stub passes, and
     * every loop index here is an `int`.  They are the same value on this ABI; the cast
     * says so once rather than at every comparison, and says it because a `-Wsign-compare`
     * that has been silenced in the build is a real class of bug papered over rather than
     * answered. */
    
    /*  is  because that is what the entry stub passes, and comparing
     * it against an  index is a sign-compare warning that is right to be noisy about:
     * the two are the same on this ABI and the cast is here to say so once. */
    long  nlines = DEFAULT_LINES;
    int   argi = 1;
    char *label = NULL;
    FILE *f = stdin;

    if (argi < nargs && strncmp(argv[argi], "-n", 2) == 0) {
        const char *digits = argv[argi] + 2;

        if (*digits == 0) {
            if (argi + 1 >= nargs) {
                fprintf(stderr, "head: -n needs a number\n");
                return 1;
            }

            digits = argv[++argi];
        }

        nlines = strtol(digits, NULL, 10);

        if (nlines < 0) {
            fprintf(stderr, "head: -n %s is not a count\n", digits);
            return 1;
        }

        argi++;
    }

    for (; argi < nargs; argi++) {
        if (argv[argi][0] == '-') {
            fprintf(stderr, "head: %s: unknown option\n", argv[argi]);
            return 1;
        }

        if (label == NULL) {
            label = argv[argi];
            f = fopen(label, "r");

            if (f == NULL) {
                fprintf(stderr, "head: %s: %s\n", label, strerror(errno));
                return 1;
            }

            continue;
        }

        fprintf(stderr, "head: only one file is supported\n");
        return 1;
    }

    long printed = 0;

    /* A count of zero prints nothing and *reads* everything, which is the same contract
     * `tail -n 0` has and for the same reason: a pipeline writing into `head -n 0` must
     * not deadlock waiting for a reader that has gone.
     *
     * The two halves matter separately.  Printing nothing is the easy half and is what the
     * option says; reading everything is the half that makes it usable in a pipeline, and
     * a loop that treats zero as "no limit" -- which is what it did -- printed the whole
     * file instead. */
    if (nlines == 0) {
        char drain[4096];

        while (fread(drain, 1, sizeof(drain), f) > 0)
            ;

        if (f != stdin)
            fclose(f);

        return ferror(stdout) ? 1 : 0;
    }

    while (printed < nlines) {
        int c = fgetc(f);

        if (c == EOF)
            break;

        putchar(c);

        if (c == '\n')
            printed++;
    }

    if (fflush(stdout) != 0 || ferror(stdout))
        return 1;

    if (f != stdin)
        fclose(f);

    return 0;
}