/* `sort` -- sort lines.
 *
 * The middle of `grep ... | sort | uniq -c | wc -l`, which is the pipeline in the Definition
 * of Done for "read a lot of text and count what is in it".
 *
 * All the lines are collected, then sorted, then printed.  That is not how a sort is
 * supposed to work -- a real one streams, and merges, and uses a bounded amount of memory --
 * and doing it this way is a deliberate choice with a stated limit: every line has to fit in
 * memory at once, so a file larger than the heap cannot be sorted by this program.  It is
 * said here rather than discovered, because "sort uses all your memory and then dies" is a
 * much worse thing to find out about than "sort holds the file in memory".
 *
 * The limit is real but generous for the case this exists for.  `shtest` sorts a directory
 * listing, and `libctest` sorts nothing larger.
 *
 * The comparison is by *byte*, which is what every `sort` does and what makes the order
 * predictable: `Z` before `a`, `10` before `9`, and a trailing newline irrelevant because
 * the terminators are not part of the keys.  `-n` is the one mode that departs from it, and
 * it departs by comparing the numbers as numbers -- which is the entire reason to have it,
 * since a byte sort puts 10 before 9 and every script that sorts numbers wants otherwise.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The options, read once by `main` and read by the comparator, which qsort cannot be given
 * an argument to.  A file-scope pair of flags is not as nice as passing a context, and it
 * is the shape that has always been available to `qsort`. */
static int opt_reverse;
static int opt_numeric;
static int opt_fold;

typedef struct {
    char **line;
    size_t  n;
    size_t  cap;
} lines_t;

/* A number, taken apart rather than evaluated.
 *
 * `-n` used to call `strtod()` and compare the results, which is the obvious way and does
 * not compile here: the user-space build is `-mno-sse`, because the kernel is freestanding
 * and the programs are built with the same flags, and a function that returns a `double`
 * returns it in an SSE register.  So the comparison is done on the digits.
 *
 * Which is better anyway.  Two doubles cannot tell `1.0000000000000001` from `1`, and a
 * sort that puts those two the wrong way round is comparing values the reader cannot see
 * either.  Comparing the digits compares what was written.
 *
 * The parts are pointers into the line, so nothing is copied and nothing is allocated. */
typedef struct {
    const char *int_part;   /* after the sign and after any leading zeros */
    size_t      int_len;
    const char *frac;       /* the digits after the '.', or NULL */
    size_t      frac_len;
    int         negative;
} num_t;

/* The canonical zero, for a line that does not begin with a number at all.
 *
 * It has to be a real "0" of length one rather than the line's own text.  Leaving `int_part`
 * pointing at the line made its *length* the number of digits, so `abc` compared as a
 * three-digit number and sorted between `10` and `007` -- which is the order the numeric
 * sort produced, and is wrong for every line that is not a number.  Every such line now
 * compares as zero and the byte comparison below decides between them, which is what
 * GNU's `sort -n` does. */
static const char number_zero[] = "0";

/* Take a leading number apart.
 *
 * Leading blanks are skipped, so `-n` sorts a column of an indented listing rather than
 * refusing because the column starts with a space.  Leading zeros are skipped too, so `007`
 * and `7` compare equal, which is what a reader expects of a *numeric* sort and not what a
 * byte sort would do.
 *
 * The leading zeros have to be skipped *before* the length is taken, not after the scan has
 * run past them: the first version advanced over all the digits and then tried to strip
 * zeros from a pointer that had already moved, so `007` kept its length of three and sorted
 * after `10`. */
static num_t parse_number(const char *s) {
    num_t       n = { number_zero, 1, NULL, 0, 0 };
    const char *p = s;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '+' || *p == '-') {
        n.negative = (*p == '-');
        p++;
    }

    if (*p < '0' || *p > '9')
        return n;                   /* no number here; it compares as zero */

    n.int_part = p;

    while (*n.int_part == '0' && n.int_part[1] >= '0' && n.int_part[1] <= '9')
        n.int_part++;

    p = n.int_part;

    while (*p >= '0' && *p <= '9')
        p++;

    n.int_len = (size_t)(p - n.int_part);

    if (*p == '.') {
        /* The scan uses a local, because walking the struct field itself leaves it pointing
         * at the *end* of the digits rather than at them.
         *
         * That is not a typo-level mistake: `frac_len` came out right, because it is
         * computed from a saved pointer, and every other field was right too.  So the number
         * compared as though its fractional part were a run of NULs, which made `2.50` sort
         * before `2.5` and `sort -n` disagree with every other `sort` on every decimal
         * number -- while the parse still *looked* correct in a debugger, because printing
         * `n.frac` with `%s` printed the empty string just past the digits and a reader
         * looking at `frac_len == 2` reads that as "two digits". */
        const char *f = p + 1;

        if (*f >= '0' && *f <= '9') {
            const char *end = f;

            while (*end >= '0' && *end <= '9')
                end++;

            n.frac = f;
            n.frac_len = (size_t)(end - f);
        }
    }

    return n;
}

/* Compare two numbers by their digits.
 *
 * The sign is applied last, because the magnitudes are what the digit comparison gives and
 * a negative number is the other way round rather than a different thing. */
static int num_cmp(const num_t *a, const num_t *b) {
    int r;

    if (a->negative != b->negative)
        return a->negative ? -1 : 1;

    /* More integer digits is a bigger number, and the leading zeros are gone, so "0" is
     * one digit like any other. */
    if (a->int_len != b->int_len)
        r = a->int_len < b->int_len ? -1 : 1;
    else
        r = strncmp(a->int_part, b->int_part, a->int_len);

    if (r == 0 && a->frac != NULL && b->frac != NULL) {
        /* Digit by digit, padding the shorter with zeros.  That is what makes `1.5` and
         * `1.50` equal rather than `1.5` sorting first. */
        for (size_t i = 0; i < a->frac_len || i < b->frac_len; i++) {
            int da = i < a->frac_len ? a->frac[i] - '0' : 0;
            int db = i < b->frac_len ? b->frac[i] - '0' : 0;

            if (da != db) {
                r = da < db ? -1 : 1;
                break;
            }
        }
    }

    return a->negative ? -r : r;
}

/* Compare two bytes ignoring case.
 *
 * `tolower()` rather than `toupper()`, so that a byte which is not a letter compares equal
 * to itself.  On this platform `tolower()` takes an `int` and returns an `int`, and the
 * argument has to be cast because a plain `char` is signed and a byte above 127 would
 * arrive as a negative number. */
static int fold_cmp(unsigned char a, unsigned char b) {
    int la = tolower((int)a);
    int lb = tolower((int)b);

    return la < lb ? -1 : la > lb ? 1 : 0;
}

/* `strcmp`, or `fold_cmp` when `-f` was given.  One function rather than a branch in the
 * comparator, because the comparator runs once per pair of lines and this is the part of
 * it that has to be right about bytes. */
static int byte_cmp(const char *a, const char *b) {
    if (!opt_fold)
        return strcmp(a, b);

    for (;; a++, b++) {
        int d = fold_cmp((unsigned char)*a, (unsigned char)*b);

        if (d != 0)
            return d;

        if (*a == 0)
            return 0;
    }
}

/* The comparison the options ask for, and nothing else.
 *
 * `-n` compares the leading numbers; without it the comparison is by bytes, folded when
 * `-f` was given.  This is "the key", and keeping it separate from the tie-break below is
 * what makes `-u` mean "one line per distinct key" rather than "one line per distinct
 * line".
 *
 * Two lines that are equal *here* may still be different lines, and the sort has to put
 * them in some order. */
static int key_cmp(const char *a, const char *b) {
    if (!opt_numeric)
        return byte_cmp(a, b);

    num_t na = parse_number(a);
    num_t nb = parse_number(b);

    return num_cmp(&na, &nb);
}

/* What happens when the keys are equal: compare the whole line.
 *
 * Plain bytes, and never folded.  `-f` is about the *key*, and folding here would change
 * the order of two lines the user asked to be considered equal: `sort -fn` on a list of
 * words, where every line has no leading number and so every key is zero, gives
 * `Banana Zebra apple` -- capital letters first, because that is what bytes are -- and not
 * the folded order.  It is also what makes `sort -n` put `2.5` before `2.50`: the two are
 * the same number, so the tie is broken by the text, and `2.5` is a prefix of `2.50`.
 *
 * Without this the order of ties is whatever `qsort` happens to produce, and it can differ
 * between runs and between machines for lines the user can see are equal. */
static int tie_cmp(const char *a, const char *b) {
    return strcmp(a, b);
}

static int cmp_line(const void *va, const void *vb) {
    const char *a = *(const char *const *)va;
    const char *b = *(const char *const *)vb;
    int         r = key_cmp(a, b);

    if (r == 0)
        r = tie_cmp(a, b);

    return opt_reverse ? -r : r;
}

static int lines_add(lines_t *v, const char *line, size_t len) {
    if (v->n == v->cap) {
        size_t want = v->cap ? v->cap * 2 : 128;
        char **grown = realloc(v->line, want * sizeof(char *));

        if (grown == NULL)
            return -1;

        v->line = grown;
        v->cap = want;
    }

    char *copy = malloc(len + 1);

    if (copy == NULL)
        return -1;

    memcpy(copy, line, len);
    copy[len] = 0;
    v->line[v->n++] = copy;
    return 0;
}

/* Add every line of a stream.
 *
 * A line longer than the buffer is *not* split: the buffer grows, so a line of any length is
 * read whole and kept as one line.  Truncating it would produce a line that sorts in the
 * wrong place and prints as something the file does not contain, and the only symptom would
 * be that the output is wrong. */
static int add_stream(FILE *f, lines_t *v) {
    size_t cap = 256;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (buf == NULL)
        return -1;

    for (;;) {
        int c = fgetc(f);

        if (c == EOF) {
            /* A last line with no terminator is still a line.  Every `sort` prints it, and
             * the one that does not is the one that quietly loses a line from every file
             * that was not edited by a text editor. */
            if (len > 0 && lines_add(v, buf, len) != 0) {
                free(buf);
                return -1;
            }

            break;
        }

        if (c == '\n') {
            if (lines_add(v, buf, len) != 0) {
                free(buf);
                return -1;
            }

            len = 0;
            continue;
        }

        if (len + 2 > cap) {
            size_t want = cap * 2;
            char  *grown = realloc(buf, want);

            if (grown == NULL) {
                free(buf);
                return -1;
            }

            buf = grown;
            cap = want;
        }

        buf[len++] = (char)c;
    }

    free(buf);
    return 0;
}

int minx_main(unsigned long argc, char **argv) {
    int nargs = (int)argc;

    int  unique = 0;
    int  argi = 1;
    int  bad = 0;

    while (argi < nargs && argv[argi][0] == '-' && argv[argi][1] != 0) {
        const char *p = argv[argi] + 1;

        if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }

        for (; *p != 0; p++) {
            if (*p == 'r') {
                opt_reverse = 1;
            } else if (*p == 'n') {
                opt_numeric = 1;
            } else if (*p == 'u') {
                unique = 1;
            } else if (*p == 'f') {
                opt_fold = 1;
            } else {
                fprintf(stderr, "sort: %s: unknown option\n", argv[argi]);
                return 1;
            }
        }

        argi++;
    }

    lines_t lines;

    memset(&lines, 0, sizeof(lines));

    if (argi >= nargs) {
        if (add_stream(stdin, &lines) != 0) {
            fprintf(stderr, "sort: out of memory\n");
            return 1;
        }
    } else {
        for (; argi < nargs; argi++) {
            FILE *f = (strcmp(argv[argi], "-") == 0) ? stdin : fopen(argv[argi], "r");

            if (f == NULL) {
                /* `cat`'s rule: reported, skipped, and the status non-zero at the end.  A
                 * sort that stopped at a missing file would leave the caller with half the
                 * answer and no way to tell which half. */
                fprintf(stderr, "sort: %s: %s\n", argv[argi], strerror(errno));
                bad = 1;
                continue;
            }

            if (add_stream(f, &lines) != 0) {
                fprintf(stderr, "sort: out of memory\n");

                if (f != stdin)
                    fclose(f);

                return 1;
            }

            if (f != stdin)
                fclose(f);
        }
    }

    if (lines.n > 1)
        qsort(lines.line, lines.n, sizeof(char *), cmp_line);

    for (size_t i = 0; i < lines.n; i++) {
        /* `-u` compares against the line before it with the *same* comparator the sort
         * used, which is why it is a loop rather than a flag in `cmp_line()`: two equal
         * lines must still be *consecutive* for one of them to go, and dropping one inside
         * the comparator would change the order of everything else.
         *
         * With `-n` this matters a good deal: every line that does not begin with a number
         * compares as zero, so `sort -nu` on a file of words prints one word rather than
         * all of them.  Comparing whole lines instead printed all of them, because the
         * words are of course different, and the whole point of `-u` is to ask about the
         * key.
         *
         * One documented difference from GNU: given two lines with equal keys, this keeps
         * the one that sorts first, and GNU keeps the one that came first in the input.
         * Every other case agrees, and this one only arises with `-u` together with keys
         * that tie, which is the single corner where GNU's rule is least obvious. */
        if (unique && i > 0 && key_cmp(lines.line[i], lines.line[i - 1]) == 0)
            continue;

        printf("%s\n", lines.line[i]);
    }

    for (size_t i = 0; i < lines.n; i++)
        free(lines.line[i]);

    free(lines.line);

    if (fflush(stdout) != 0 || ferror(stdout))
        return 1;

    return bad;
}