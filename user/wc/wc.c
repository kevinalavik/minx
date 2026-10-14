/* `wc` -- count lines, words and characters.
 *
 * One of the six programs `shtest`'s script needs, and the only one of them it uses for a
 * number: `echo "lines=$(wc -l < /tmp/x)"` checks that an append really appended, and a
 * shell test that cannot count lines cannot test a redirection.
 *
 * Written as a filter over a byte at a time rather than with `fgets()`, because the
 * definition of a line is "a `\n`" and a buffer whose last line has no `\n` is not a line
 * on every platform.  A missing trailing newline is the classic off-by-one here: `printf a`
 * into a file and `wc -l` must say 0, because there is no line there yet, only one line
 * waiting for a terminator that has not arrived.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 4096

static void tally(const char *buf, size_t n, unsigned long *lines, unsigned long *words,
                  unsigned long *bytes) {
    int in_word = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];

        *bytes += 1;

        if (c == '\n')
            *lines += 1;

        /* A word is a run of non-blanks, and what counts as a blank is the shell's idea of
         * one rather than the C library's: `isblank()` for the space and the tab, and the
         * newline counted separately because it is also a line break.  Anything else is a
         * word character, which is why `wc -w` on `a,b` says 1. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            in_word = 0;
            continue;
        }

        if (!in_word) {
            in_word = 1;
            *words += 1;
        }
    }
}

/* How wide each count is printed.
 *
 * GNU `wc` right-aligns every count in a field of seven when there is more than one count
 * or a file name to go with it, and prints a single count bare when there is neither:
 * `wc -l -w f` prints "  2  2 f" and `wc -l < f` prints "2".
 *
 * That difference is not cosmetic.  The script under test does
 * `echo "lines=$(wc -l < f)"`, and a command substitution inside double quotes is one word
 * -- so a padded count arrives as seven leading blanks followed by the number, and the
 * line under test reads `lines=       2` where it should read `lines=2`.  The count was
 * right; the column was not, and a column is not part of an answer.
 *
 * `width` is the whole field including the separator that follows it, so it is 8 when
 * padded and 1 when not.
 */
static unsigned count_width(int numbers, int has_name) {
    return (numbers > 1 || has_name) ? 8 : 1;
}

static void print_counts(unsigned long lines, unsigned long words, unsigned long bytes,
                         const int *want, unsigned width) {
    if (want[0]) printf("%*lu", (int)width, lines);
    if (want[1]) printf("%*lu", (int)width, words);
    if (want[2]) printf("%*lu", (int)width, bytes);
}

/* How many of the three counts this invocation is printing. */
static int want_count(const int *want) {
    return (want[0] ? 1 : 0) + (want[1] ? 1 : 0) + (want[2] ? 1 : 0);
}

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
    /* `argc` is `unsigned long` because that is what the entry stub passes, and every loop
     * index below is an `int`.  They are the same value on this ABI; the cast says so once
     * rather than at every comparison, and says it because a `-Wsign-compare` that has been
     * silenced in the build is a real class of bug papered over rather than answered. */
    int nargs = (int)argc;

    int want[3] = { 0, 0, 0 };
    int named = 0;
    int i = 1;

    unsigned long tl = 0, tw = 0, tb = 0;

    /* `-l`, `-w`, `-c` select what is printed; no option at all prints all three, in that
     * order, which is the default every script assumes when it does `wc` and reads the
     * first number. */
    for (; i < nargs; i++) {
        if (strcmp(argv[i], "-l") == 0) { want[0] = 1; continue; }
        if (strcmp(argv[i], "-w") == 0) { want[1] = 1; continue; }
        if (strcmp(argv[i], "-c") == 0) { want[2] = 1; continue; }
        break;
    }

    if (!want[0] && !want[1] && !want[2])
        want[0] = want[1] = want[2] = 1;

    /* Standard input: one line of output, no name, so a single count is bare. */
    if (i >= nargs) {
        char   buf[CHUNK];
        size_t n;
        int    failed = 0;

        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
            tally(buf, n, &tl, &tw, &tb);

        if (ferror(stdin))
            failed = 1;

        print_counts(tl, tw, tb, want, count_width(want_count(want), 0));
        putchar('\n');

        return failed ? 1 : 0;
    }

    /* The name is printed for every file that was named, which is what makes `wc f` say
     * "2 f" and `wc -l < f` -- no name -- say "2". */
    unsigned width = count_width(want_count(want), 1);

    for (; i < nargs; i++) {
        FILE         *f = fopen(argv[i], "r");
        char          buf[CHUNK];
        size_t        n;
        unsigned long l = 0, w = 0, b = 0;

        named++;

        if (f == NULL) {
            /* `cat`'s rule: reported, skipped, and the status is non-zero at the end. */
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            tl = tw = tb = (unsigned long)-1;
            continue;
        }

        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            tally(buf, n, &l, &w, &b);

        fclose(f);

        print_counts(l, w, b, want, width);
        printf(" %s", argv[i]);
        putchar('\n');

        tl += l;
        tw += w;
        tb += b;
    }

    /* The total, for more than one file.  It is not printed for one: `wc -l f` printing
     * "3 f" and then "3 total" is two lines for one fact. */
    if (named > 1) {
        print_counts(tl, tw, tb, want, width);
        printf(" total\n");
    }

    return ferror(stdout) ? 1 : 0;
}
