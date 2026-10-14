/* `tail` -- the last lines of a file.
 *
 * One of the six programs `shtest`'s script needs: `echo "tail=$(tail -n 1 /tmp/x)"` reads
 * back the line an append just added, which is how the script checks that `>>` appended
 * rather than overwriting.
 *
 * The default is the last *ten* lines and `-n N` overrides it in both spellings, because
 * both are in use by scripts and the short form is the one the test needs.
 *
 * A ring of *lines*, not of bytes.  A byte ring has to track where each line starts inside
 * itself, which is the part that is easy to get wrong: the first version kept a start
 * offset, a length and a "partial" flag, and printed NUL bytes for a two-line file, because
 * the offset it walked forward from was computed from a length that had already been
 * overwritten.  Storing the lines themselves and rotating a pointer ring makes the whole
 * question disappear -- the oldest line is simply the one the next write will overwrite,
 * and there is nothing to compute.
 *
 * A line longer than memory is still memory this program uses, which is the one thing a
 * `tail` of a large file must not do; the bound is on the *number* of lines, and that is
 * the part that grows with the request rather than with the input.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_LINES 10

/* Read one line, without the terminator, into a buffer this owns.
 *
 * `getline()` is not used because the shell's own libc has no `getdelim()` and a program
 * that has to grow its buffer by hand should do it once, here, rather than in every caller.
 * Returns NULL at end of input with nothing read, and otherwise a line with its terminator
 * stripped -- including for a last line that has none, because `tail` shows what is there
 * where `wc -l` counts terminators.  `*had_newline` says which it was. */
static char *read_line(FILE *f, size_t *len, int *had_newline) {
    size_t cap = 128;
    size_t n = 0;
    char  *b = malloc(cap);

    if (b == NULL)
        return NULL;

    for (;;) {
        int c = fgetc(f);

        if (c == EOF)
            break;

        if (c == '\n') {
            *had_newline = 1;
            break;
        }

        if (n + 2 > cap) {
            size_t want = cap * 2;
            char  *grown = realloc(b, want);

            if (grown == NULL) {
                free(b);
                return NULL;
            }

            b = grown;
            cap = want;
        }

        b[n++] = (char)c;
    }

    if (n == 0 && !*had_newline) {
        free(b);
        return NULL;
    }

    b[n] = 0;
    *len = n;
    return b;
}

int minx_main(unsigned long argc, char **argv) {
    /* `argc` is `unsigned long` because that is what the entry stub passes, and every index
     * below is an `int`.  They are the same value on this ABI; the cast says so once rather
     * than at every comparison, and says it because a `-Wsign-compare` that has been
     * silenced in the build is a real class of bug papered over rather than answered. */
    int nargs = (int)argc;
    long nlines = DEFAULT_LINES;
    int  argi = 1;
    char *label = NULL;
    FILE *f = stdin;

    if (argi < nargs && strncmp(argv[argi], "-n", 2) == 0) {
        const char *digits = argv[argi] + 2;

        if (*digits == 0) {
            if (argi + 1 >= nargs) {
                fprintf(stderr, "tail: -n needs a number\n");
                return 1;
            }

            digits = argv[++argi];
        }

        /* A negative count is how every `tail` spells "all but the last N", and it is not
         * implemented: saying so is better than printing the last N lines, which is what
         * dropping the sign would do. */
        if (digits[0] == '-') {
            fprintf(stderr, "tail: -n +%s (all but the last) is not implemented\n",
                    digits + 1);
            return 1;
        }

        nlines = strtol(digits, NULL, 10);

        if (nlines < 0) {
            fprintf(stderr, "tail: -n %s is not a count\n", digits);
            return 1;
        }

        argi++;
    }

    for (; argi < nargs; argi++) {
        if (argv[argi][0] == '-') {
            fprintf(stderr, "tail: %s: unknown option\n", argv[argi]);
            return 1;
        }

        if (label == NULL) {
            label = argv[argi];
            f = fopen(label, "r");

            if (f == NULL) {
                fprintf(stderr, "tail: %s: %s\n", label, strerror(errno));
                return 1;
            }

            continue;
        }

        fprintf(stderr, "tail: only one file is supported\n");
        return 1;
    }

    /* Zero lines: the output is nothing, and the input still has to be read -- or a
     * pipeline writing into `tail -n 0` deadlocks, which is the opposite of what the caller
     * asked for and the opposite of what every `tail` does. */
    if (nlines == 0) {
        char drain[4096];

        while (fread(drain, 1, sizeof(drain), f) > 0)
            ;

        if (f != stdin)
            fclose(f);

        return ferror(stdout) ? 1 : 0;
    }

    char **ring = calloc((size_t)nlines, sizeof(char *));

    if (ring == NULL)
        return 1;

    long  next = 0;      /* where the next line goes: the oldest one, once it is full */
    long  held = 0;      /* how many lines the ring has in it                        */
    int   status = 0;

    for (;;) {
        size_t len = 0;
        int    nl = 0;
        char  *line = read_line(f, &len, &nl);

        if (line == NULL)
            break;

        /* The line the next write replaces is the one that is leaving the window.  Freeing
         * it here rather than at the end is what keeps the memory bounded by the window
         * rather than by the file. */
        free(ring[next]);
        ring[next] = line;
        next = (next + 1) % nlines;

        if (held < nlines)
            held++;
    }

    if (ferror(f)) {
        fprintf(stderr, "tail: read: %s\n", strerror(errno));
        status = 1;
    }

    /* Where the oldest line is.
     *
     * Once the ring is full, that is the slot the next write goes to -- it holds the line
     * that has just been pushed out of the window.  Before it is full it is slot 0, because
     * the writes have not started wrapping yet.
     *
     * Using `next` in both cases is what made `tail` of a file *shorter* than the window
     * print nothing at all: with a three-line file and the default window of ten, `next`
     * was 3 and the loop started at `ring[3]`, which is empty.  The window and the answer
     * are the same shape here -- a file shorter than the window prints all of it -- and
     * this is the one place that has to say so. */
    long start = (held == nlines) ? next : 0;

    for (long i = 0; i < held; i++) {
        const char *line = ring[(start + i) % nlines];

        if (line == NULL)
            continue;

        fputs(line, stdout);
        putchar('\n');
    }

    for (long i = 0; i < held; i++)
        free(ring[i]);

    free(ring);

    if (f != stdin)
        fclose(f);

    if (fflush(stdout) != 0 || ferror(stdout))
        status = 1;

    return status;
}