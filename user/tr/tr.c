/* `tr` -- translate, delete or squeeze characters.
 *
 * One of the three tools `echo hi | tr a-z A-Z` needs, and the reason that line is in the
 * Definition of Done is that it is the shortest thing in a shell that proves a *pipe*
 * carries data between two programs that know nothing about each other.
 *
 * Two character sets and a 256-entry table.
 *
 * A set is a string of literals, ranges (`a-z`) and backslash escapes, expanded once up
 * front rather than matched as the input streams past.  The alternative is to interpret
 * the set again for every byte, and the set is often longer than the input -- `tr -d ' \t\n'`
 * over a big file would spend all its time re-parsing four characters.
 *
 * Delete and squeeze are separate 256-bit maps rather than extra columns in the translate
 * table, because they are different questions and folding them in would make every mode pay
 * for the other two's bookkeeping.
 *
 * When SET2 is shorter than SET1 its last byte is used for the rest.  That is what every
 * `tr` does, and it is the only rule that makes `tr abc xy` mean anything: the intent is
 * obviously that `a` and `b` become `x` and `y` and `c` becomes `y` too.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NSET 256

typedef struct {
    unsigned char to[NSET];   /* the byte this one becomes, for translate        */
    unsigned char del[NSET];  /* 1 if this byte is deleted                      */
    unsigned char sqz[NSET];  /* 1 if repeats of this byte are squeezed         */
} tr_sets_t;

/* Expand a set specification into the bytes it names, *in order*.
 *
 * Order matters: SET2 is positional, and `tr abc xyz` has to map a->x, b->y and c->z.  Read
 * as an unordered set all three would map to `x`, which is a different command that looks
 * identical on the command line.
 *
 * The escapes are the part worth naming, because the first version had none of them and
 * `tr -d '\n'` then deleted the letters `\` and `n` -- a silently different command that
 * on a text file removes the `n` out of every word in it.
 *
 *   \n \t \r \f \v \b \a   the usual control names
 *   \\                    a backslash, which is the only way to name one
 *   \ooo                  up to three octal digits, which is the only way to name a NUL
 *
 * Returns the number of bytes, or -1 with a message already printed.
 *
 * `limit` is NSET for a membership set, where duplicates are collapsed by the caller, and
 * the set's own length for SET2, where a repeat is a separate entry that consumes the
 * corresponding byte of SET1 -- which is what makes `tr aab xy` map both `a`s to `x` and
 * `b` to `y`. */
static int set_bytes(const char *spec, const char *what, unsigned char *out, int limit) {
    int    n = 0;
    size_t len = strlen(spec);

    for (size_t i = 0; i < len && n < limit; i++) {
        unsigned char c = (unsigned char)spec[i];

        if (c == '\\' && i + 1 < len) {
            char e = spec[++i];

            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'f': c = '\f'; break;
            case 'v': c = '\v'; break;
            case 'b': c = '\b'; break;
            case 'a': c = '\a'; break;
            case '\\': c = '\\'; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                unsigned val = (unsigned)(e - '0');

                /* Up to three octal digits and no more, so `\0123` is a newline and then a
                 * `3` rather than an error about an out-of-range byte. */
                for (int k = 0; k < 2 && i + 1 < len && spec[i + 1] >= '0' &&
                                spec[i + 1] <= '7'; k++)
                    val = val * 8 + (unsigned)(spec[++i] - '0');

                c = (unsigned char)(val & 0xff);
                break;
            }
            default:
                /* An unknown escape names the character itself, which is what every `tr`
                 * does and the only rule that cannot be wrong about what was meant. */
                c = (unsigned char)e;
                break;
            }
        }

        /* A range needs one more character after the `-`, and a `-` at the end of the set
         * is a literal `-` rather than the start of a range that runs off the end. */
        if (i + 2 < len && spec[i + 1] == '-' && spec[i + 2] != '\\') {
            unsigned char hi = (unsigned char)spec[i + 2];

            /* Descending is a mistake, not a request to wrap: every implementation agrees
             * `tr z-a` names nothing, and silently translating 26 characters the wrong way
             * round is worse than saying so. */
            if (hi < c) {
                fprintf(stderr, "tr: %s: '%c-%c' is not a range\n", what, c, hi);
                return -1;
            }

            for (unsigned v = c; v <= hi && n < limit; v++)
                out[n++] = (unsigned char)v;

            i += 2;
            continue;
        }

        out[n++] = c;
    }

    return n;
}

int minx_main(unsigned long argc, char **argv) {
    int nargs = (int)argc;

    int del = 0;
    int squeeze = 0;
    int complement = 0;
    int argi = 1;

    /* Options are bundled, as POSIX has them, so `-ds` is `-d -s`.  The first non-option
     * ends them, which is why a set that begins with `-` has to be written after `--`. */
    while (argi < nargs && argv[argi][0] == '-' && argv[argi][1] != 0) {
        const char *p = argv[argi] + 1;

        if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }

        for (; *p != 0; p++) {
            if (*p == 'd') {
                del = 1;
            } else if (*p == 's') {
                squeeze = 1;
            } else if (*p == 'c' || *p == 'C') {
                complement = 1;
            } else {
                fprintf(stderr, "tr: %s: unknown option\n", argv[argi]);
                return 1;
            }
        }

        argi++;
    }

    if (argi >= nargs) {
        fprintf(stderr, "tr: missing operand\n");
        return 1;
    }

    tr_sets_t sets;

    memset(&sets, 0, sizeof(sets));

    /* The identity table, in every mode, so that the output loop below never has to ask
     * which mode it is in.
     *
     * It used to ask: `o = (deleted || squeezed) ? c : sets.to[c]`, which is the wrong way
     * round for a byte that *survived* -- that one has neither bit set, so it fell through
     * to `sets.to[c]`, and in `-d` and `-s` that table was never filled and read zero.  So
     * `tr -d e` wrote a NUL for every newline and `tr -s ' '` wrote a NUL for every `a`.
     * The output was the right *length* and the wrong content, which is the hardest kind of
     * wrong to notice.
     *
     * Translate then overwrites the entries it has something to say about, and `-d`/`-s`
     * leave this alone. */
    for (unsigned v = 0; v < NSET; v++)
        sets.to[v] = (unsigned char)v;

    unsigned char in[NSET];
    int           n_in = set_bytes(argv[argi], argv[argi], in, NSET);

    if (n_in < 0)
        return 1;

    unsigned char member[NSET];

    memset(member, 0, sizeof(member));

    for (int i = 0; i < n_in; i++)
        member[in[i]] = 1;

    if (complement) {
        for (unsigned i = 0; i < NSET; i++)
            member[i] = member[i] ? 0 : 1;
    }

    if (del) {
        memcpy(sets.del, member, sizeof(sets.del));
    } else if (squeeze) {
        memcpy(sets.sqz, member, sizeof(sets.sqz));
    } else {
        if (argi + 1 >= nargs) {
            fprintf(stderr, "tr: two strings are required for translation\n");
            return 1;
        }

        unsigned char out_set[NSET];
        int n_out = set_bytes(argv[argi + 1], argv[argi + 1], out_set, NSET);

        if (n_out < 0)
            return 1;

        if (n_out == 0) {
            fprintf(stderr, "tr: the second string is empty\n");
            return 1;
        }

        unsigned char last = out_set[n_out - 1];
        int           k = 0;

        for (unsigned v = 0; v < NSET; v++) {
            if (!member[v])
                continue;

            sets.to[v] = (k < n_out) ? out_set[k] : last;
            k++;
        }
    }

    /* Read in chunks and squeeze across the boundary.
     *
     * The last byte of each chunk is *decided* but not emitted, because whether it survives
     * depends on whether the next chunk starts with the same byte.  A version that emitted
     * every byte it decided turned `tr -s` on a file whose repeated characters happen to
     * straddle a read into one character per read -- right on small files and wrong on
     * large ones, which is the worst place for it to be wrong.
     *
     * Output is compacted forward through the same buffer rather than into a second one,
     * because deleting and squeezing both remove bytes and so the output is not a prefix
     * of the input. */
    char   buf[8192];
    size_t pending = 0;
    int    last_out = -1;

    for (;;) {
        ssize_t got = read(0, buf + pending, sizeof(buf) - pending);

        if (got < 0) {
            fprintf(stderr, "tr: read: %s\n", strerror(errno));
            return 1;
        }

        if (got == 0)
            break;

        size_t have = pending + (size_t)got;
        size_t decide = have - 1;       /* the last byte waits for its successor */
        size_t w = 0;

        for (size_t i = 0; i < decide; i++) {
            unsigned char c = (unsigned char)buf[i];

            if (sets.del[c])
                continue;

            if (sets.sqz[c] && (int)c == last_out)
                continue;

            buf[w++] = (char)sets.to[c];
            last_out = sets.to[c];
        }

        if (w > 0 && write(1, buf, w) != (ssize_t)w) {
            fprintf(stderr, "tr: write: %s\n", strerror(errno));
            return 1;
        }

        memmove(buf, buf + decide, have - decide);
        pending = have - decide;
    }

    /* The bytes that were decided but held back, now that there is no successor to change
     * their minds about. */
    {
        size_t w = 0;

        for (size_t i = 0; i < pending; i++) {
            unsigned char c = (unsigned char)buf[i];

            if (sets.del[c])
                continue;

            if (sets.sqz[c] && (int)c == last_out)
                continue;

            buf[w++] = (char)sets.to[c];
            last_out = sets.to[c];
        }

        if (w > 0 && write(1, buf, w) != (ssize_t)w)
            return 1;
    }

    return 0;
}