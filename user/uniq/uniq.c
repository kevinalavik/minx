/* `uniq` -- drop adjacent duplicate lines.
 *
 * The end of `grep ... | sort | uniq -c | wc -l`: `uniq` counts what is left, and `wc`
 * counts the lines that come out.  `sort` first is what makes it work at all -- `uniq`
 * compares each line with the one *before* it, so an input that has not been sorted has its
 * duplicates in different places and `uniq` will not see them.
 *
 * Only adjacent.  That is not a limitation this program could remove by trying harder: it
 * is what `uniq` means, on every Unix, and the alternative -- comparing every line with
 * every other -- is a different program that costs O(n squared) memory and time.  A caller
 * that wants the other one asks for a `sort` first, which is why the pipeline above starts
 * with one.
 *
 * So the whole program is a one-line-lookback, and the count is a counter.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Options.  `read whole file` rather than `read one byte` because the comparison is a
 * `strcmp` and the alternative is a function call per byte; and the buffer grows, so a line
 * of any length is one line. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} line_t;

/* Put a copy of `src` into `l`, growing it and allocating it if this is the first line.
 *
 * The previous line has to *survive* the next one being read into a different buffer, so
 * it cannot be the buffer being filled -- and it has to be allocated before it can hold
 * anything.  The first version copied into it unconditionally, which is a `memcpy` to a NULL
 * pointer on the very first line of every input. */
static int line_set(line_t *l, const char *src, size_t len) {
    if (len + 1 > l->cap) {
        size_t want = l->cap ? l->cap * 2 : 256;
        char  *grown = realloc(l->buf, want);

        if (grown == NULL)
            return -1;

        l->buf = grown;
        l->cap = want;
    }

    memcpy(l->buf, src, len + 1);
    l->len = len;
    return 0;
}

static int line_byte(line_t *l, int c) {
    if (l->len + 2 > l->cap) {
        size_t want = l->cap ? l->cap * 2 : 256;
        char  *grown = realloc(l->buf, want);

        if (grown == NULL)
            return -1;

        l->buf = grown;
        l->cap = want;
    }

    l->buf[l->len++] = (char)c;
    return 0;
}

/* What to print for a run of `count` copies of `line`.
 *
 * `-d` prints only the lines that appeared more than once and `-u` only those that
 * appeared once, and with neither every run is printed once.  Both are "which runs", not
 * "which lines": with `-d`, a line that appears three times is printed once, not twice,
 * which is the only answer that makes the output a *filtered* list rather than a summary. */
static int wanted(unsigned long count, int only_dupes, int only_uniques) {
    if (only_dupes)
        return count > 1;

    if (only_uniques)
        return count == 1;

    return 1;
}

int minx_main(unsigned long argc, char **argv) {
    int nargs = (int)argc;

    int count_mode = 0;
    int only_dupes = 0;
    int only_uniques = 0;
    int argi = 1;

    while (argi < nargs && argv[argi][0] == '-' && argv[argi][1] != 0) {
        const char *p = argv[argi] + 1;

        if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }

        for (; *p != 0; p++) {
            if (*p == 'c') {
                count_mode = 1;
            } else if (*p == 'd') {
                only_dupes = 1;
            } else if (*p == 'u') {
                only_uniques = 1;
            } else {
                fprintf(stderr, "uniq: %s: unknown option\n", argv[argi]);
                return 1;
            }
        }

        argi++;
    }

    /* One file, or standard input.  Two files is not offered: `uniq a b` would have to
     * concatenate them to find the runs, and a file whose last line has no terminator would
     * then join to the first file's first line, which is a question about two files
     * pretending to be one.  `cat a b | uniq` says what it means. */
    FILE *f = stdin;

    if (argi < nargs) {
        if (nargs - argi > 1) {
            fprintf(stderr, "uniq: only one input file is supported\n");
            return 1;
        }

        f = fopen(argv[argi], "r");

        if (f == NULL) {
            fprintf(stderr, "uniq: %s: %s\n", argv[argi], strerror(errno));
            return 1;
        }
    }

    line_t  cur;
    line_t  prev;
    int     have_prev = 0;
    int     failed = 0;

    memset(&cur, 0, sizeof(cur));
    memset(&prev, 0, sizeof(prev));

    unsigned long run = 0;

    /* Read the whole input as a sequence of lines and fold each one into the current run.
     *
     * The alternative -- comparing each line against the previous one as it is read -- is
     * the same thing with one fewer buffer, and it is wrong in the way that matters here:
     * the previous line has to *survive* the comparison, so it cannot be the buffer being
     * filled. */
    for (;;) {
        int c = fgetc(f);

        if (c == EOF) {
            if (ferror(f)) {
                fprintf(stderr, "uniq: read: %s\n", strerror(errno));
                failed = 1;
            }

            break;
        }

        if (c != '\n') {
            if (line_byte(&cur, c) != 0) {
                fprintf(stderr, "uniq: out of memory\n");
                failed = 1;
                break;
            }

            continue;
        }

        cur.buf[cur.len] = 0;

        if (!have_prev) {
            if (line_set(&prev, cur.buf, cur.len) != 0) {
                fprintf(stderr, "uniq: out of memory\n");
                failed = 1;
                break;
            }

            have_prev = 1;
        } else if (strcmp(cur.buf, prev.buf) == 0) {
            run++;
        } else {
            /* A different line: the run that has been counted is finished, so it is printed
             * before the new line becomes the one being counted. */
            if (wanted(run + 1, only_dupes, only_uniques)) {
                if (count_mode)
                    printf("%7lu %s\n", run + 1, prev.buf);
                else
                    printf("%s\n", prev.buf);
            }

            if (line_set(&prev, cur.buf, cur.len) != 0) {
                fprintf(stderr, "uniq: out of memory\n");
                failed = 1;
                break;
            }

            run = 0;
        }

        cur.len = 0;
    }

    /* A last line with no terminator is still a line, and it is still the end of a run.
     *
     * Skipping it loses the final line of every file that was not edited by a text editor,
     * which is most files a program writes.  The three cases mirror the loop above exactly:
     * the first copy of a line sets the count to one, an equal line adds one to it, and a
     * different one flushes the run and starts a new one.
     *
     * `run` counts *repeats*, not lines -- the first line of a run leaves it at zero -- so
     * the count printed is always `run + 1`.  Adding one to `run` for the very first line
     * is what made `uniq -c` report 2 for a file holding one line. */
    if (!failed && cur.len > 0) {
        cur.buf[cur.len] = 0;

        if (!have_prev) {
            if (line_set(&prev, cur.buf, cur.len) != 0) {
                fprintf(stderr, "uniq: out of memory\n");
                failed = 1;
            }

            have_prev = 1;
        } else if (strcmp(cur.buf, prev.buf) == 0) {
            run++;
        } else {
            if (wanted(run + 1, only_dupes, only_uniques)) {
                if (count_mode)
                    printf("%7lu %s\n", run + 1, prev.buf);
                else
                    printf("%s\n", prev.buf);
            }

            if (line_set(&prev, cur.buf, cur.len) != 0) {
                fprintf(stderr, "uniq: out of memory\n");
                failed = 1;
            } else {
                run = 0;
            }
        }
    }

    if (have_prev && !failed && wanted(run + 1, only_dupes, only_uniques)) {
        if (count_mode)
            printf("%7lu %s\n", run + 1, prev.buf);
        else
            printf("%s\n", prev.buf);
    }

    free(cur.buf);
    free(prev.buf);

    if (f != stdin)
        fclose(f);

    if (fflush(stdout) != 0 || ferror(stdout))
        failed = 1;

    return failed;
}