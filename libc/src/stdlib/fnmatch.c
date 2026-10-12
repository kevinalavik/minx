/* minx - shell pattern matching.
 *
 * `*`, `?` and `[...]`, and the three rules that make them behave the way a shell user
 * expects rather than the way a regular expression would.
 *
 * The matcher is recursive and the recursion is on the *pattern*, not on the string: each
 * step consumes one pattern element and then asks "can the rest match the rest?".  That is
 * the shape that makes `*` cheap to reason about, and it is also the shape that makes naive
 * matching quadratic -- `*a*a*a*a*a*a*b` against a long run of `a`s tries every split.  A
 * shell user will not type that; a program looping over a directory of a thousand files
 * might, and the cost of one pathological pattern is bounded here by the backtracking rule
 * at the `*` below, which is why that comment is where it is.
 */
#include <fnmatch.h>
#include <string.h>

/* Fold for comparison.
 *
 * ASCII only, deliberately.  Case folding in a C library is a locale question, and this libc
 * has no locale: `tolower()` is the identity outside the C locale, and a pattern that
 * matched case-insensitively on one machine and not another would be worse than one that
 * never does.  Restricted to letters so that `FNM_CASEFOLD` cannot make `[a-z]` match a
 * punctuation character -- the classic way a naive `| 0x20` fold goes wrong. */
static int fold(int c, int flags) {
    if (!(flags & FNM_CASEFOLD))
        return c;

    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';

    return c;
}

/* One step of the matcher.
 *
 * `p` is the pattern, `s` the string, and both are walked in place; the answer is whether
 * the whole of `s` is consumed.  `flags` is passed down rather than reached for because
 * <fnmatch.h> is a public header and this file is the only thing that should have to know
 * how the recursion is shaped. */
static int match(const char *p, const char *s, int flags) {
    /* The end of the pattern matches the end of the string and nothing else.  This is the
     * base case, and it is reached by every successful match. */
    if (*p == 0)
        return *s == 0;

    /* A `*` is as many characters as it likes, including none -- including a `/` unless
     * `FNM_PATHNAME` says otherwise, and including a leading `.` unless `FNM_PERIOD` says
     * otherwise. */
    if (*p == '*') {
        /* The two "may not match this" rules, and they are about what the *string* looks
         * like at this point rather than about the pattern.  Checking them once here, where
         * the star is, is what makes them hold for every expansion of it: a `*` that may not
         * swallow the leading dot of a hidden file must not swallow it by matching it as an
         * ordinary character a moment later either. */
        int blocked = 0;

        if (flags & FNM_PATHNAME)
            blocked = (*s == '/');

        if ((flags & (FNM_PATHNAME | FNM_PERIOD)) && *s == '.')
            blocked = 1;

        if (blocked)
            return 0;

        /* Try every number of characters the star can eat, shortest first, so that a
         * pattern that matches without the star expanding prefers that.  The `FNM_PATHNAME`
         * case stops at the first `/` instead of trying to cross it, which is what keeps
         * `*` from matching a whole path: the loop below would otherwise find, at enormous
         * cost, that `*` can cover `a/b/c` and leave the pattern satisfied.
         *
         * Skipping the slash means `*` never crosses a directory boundary, so `a*b` cannot
         * match `a/b` -- which is what makes `find -name '*.c'` find files and not
         * directories that happen to end in `.c`. */
        for (const char *t = s; ; t++) {
            if (match(p + 1, t, flags))
                return 1;

            if (*t == 0)
                return 0;

            if ((flags & FNM_PATHNAME) && t[1] == '/')
                return 0;
        }
    }

    /* A `?` is exactly one character -- but not a `/` under `FNM_PATHNAME`, and not the
     * leading `.` of a hidden file under the period rules. */
    if (*p == '?') {
        if (*s == 0)
            return 0;

        if ((flags & FNM_PATHNAME) && *s == '/')
            return 0;

        if ((flags & (FNM_PATHNAME | FNM_PERIOD)) && *s == '.')
            return 0;

        return match(p + 1, s + 1, flags);
    }

    /* A `[` opens a set, unless it is the whole name of a character -- `[` and `[]` are
     * ordinary characters, which is why this looks at what follows rather than just
     * matching the bracket. */
    if (*p == '[') {
        const char *q = p + 1;
        int negate = 0;
        int found  = 0;

        /* A `!` or a `^` immediately after the bracket negates.  Both spellings are
         * accepted because both exist: POSIX says `!`, every shell accepts `^`, and a user
         * who has used one of them should not have to know which. */
        if (*q == '!' || *q == '^') {
            negate = 1;
            q++;
        }

        /* A `]` in the first position is a member of the set, not the end of it -- otherwise
         * there would be no way to match a literal `]` at all. */
        if (*q == ']') {
            if (*s == ']')
                found = 1;

            q++;
        }

        for (; *q != 0 && *q != ']'; q++) {
            char lo = q[0];

            /* A range, `a-z`.  The `-` is only a range marker when it is between two
             * characters; a trailing one is a literal, which is why the loop stops on `]` or
             * the end and not on `-`. */
            if (q[1] == '-' && q[2] != 0 && q[2] != ']') {
                char hi = q[2];
                int  c  = fold(*s, flags);

                /* A reversed range, `z-a`, matches nothing rather than everything.  The
                 * alternative is to swap the ends, which is what a naive implementation
                 * does and which turns a typo into a pattern that matches every file. */
                if (lo <= hi) {
                    if (c >= fold(lo, flags) && c <= fold(hi, flags))
                        found = 1;
                } else {
                    if (c <= fold(lo, flags) || c >= fold(hi, flags))
                        found = 1;
                }

                q += 2;
                continue;
            }

            if (fold(*s, flags) == fold(lo, flags))
                found = 1;
        }

        /* An unterminated `[` is not a set at all: `[abc` is three ordinary characters,
         * and a pattern that treated the bracket as a set would match `[` itself and
         * silently change the meaning of a file name. */
        if (*q != ']')
            return fold(*s, flags) == fold('[', flags) && match(p + 1, s + 1, flags);

        if (found == negate)
            return 0;

        return match(q + 1, s + 1, flags);
    }

    /* Everything else is a literal, possibly escaped. */
    {
        char pc = *p;

        if (pc == '\\' && !(flags & FNM_NOESCAPE)) {
            pc = p[1];

            /* A trailing backslash escapes nothing, because there is nothing after it.  It
             * is treated as a literal backslash rather than as a pattern that cannot
             * match, which is what a user who typed a trailing `\` meant. */
            if (pc == 0)
                pc = '\\';
            else
                p++;
        }

        if (fold(*s, flags) != fold(pc, flags))
            return 0;

        return match(p + 1, s + 1, flags);
    }
}

int fnmatch(const char *pattern, const char *string, int flags) {
    if (pattern == NULL || string == NULL)
        return FNM_NOMATCH;

    return match(pattern, string, flags) ? 0 : FNM_NOMATCH;
}
