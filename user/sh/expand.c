/* minx - the expander.
 *
 * A word as it was typed into the fields it stands for.  This is where quoting lives, where
 * `$x` becomes text, where `*` becomes a list of file names, and where the difference
 * between "one word" and "several" is decided.
 *
 * The order of the four things is the whole of the POSIX rules and none of it is
 * arbitrary:
 *
 *   1. **Tilde**, at the very start of a word.  First, because `~` is a home directory and
 *      `"~"` is a literal tilde, and because nothing later can make a tilde that is not at
 *      the start be one.
 *   2. **Parameter, command and arithmetic expansion**, producing a string.
 *   3. **Quote removal**, which is really "marking what was protected" -- see below.
 *   4. **Field splitting** on IFS, then **pathname expansion**.  Last, and in that order,
 *      because a glob that produced fields would then be split again, and `x=*` has to end
 *      up as one value made of all the matches rather than as several values.
 *
 * **The quoting mark.**  Steps 2 and 3 are merged here into one pass that records, for
 * every character it produces, whether that character was protected by a quote or an escape.
 * The alternative -- removing the marks and then splitting -- cannot tell
 * `echo "a b"c` (one word, `a bc`) from `echo a" "b` (one word, `a b`, for the same reason)
 * from `x=$y` (however many fields `$y` has).  All three produce a string with a space in
 * it and only one of them is right to split it.
 *
 * So the buffer carries a parallel array of one-bit flags, splitting only breaks at
 * *unquoted* whitespace, and a field that contained any protected character is not a
 * pattern.  That last part is what makes `"*.c"` a file name and `*.c` a glob.
 */
#include "sh.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* From exec.c: run a command line and give back its standard output.  Declared here rather
 * than in sh.h because it is the one function in this file that needs a process, and the
 * expander should not have to know what a process is. */
char *sh_run_capture(const char *text);

/* --- the buffer, with its quoting marks ---------------------------------------- */

typedef struct {
    char  *p;       /* the text                     */
    char  *q;       /* one byte per character: 1 = protected by a quote or an escape */
    size_t n;
    size_t cap;
} sbuf_t;

static void sb_init(sbuf_t *b) {
    memset(b, 0, sizeof(*b));
}

static void sb_free(sbuf_t *b) {
    free(b->p);
    free(b->q);
    memset(b, 0, sizeof(*b));
}

static void sb_putc(sbuf_t *b, char c, int quoted) {
    if (b->n + 2 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 32;
        char  *gp = realloc(b->p, want);
        char  *gq = realloc(b->q, want);

        if (gp == NULL || gq == NULL) {
            /* Out of memory.  Keeping the text and losing the flag is better than losing
             * the text: a word with the wrong quoting is still a word, and a shell that
             * freed its own buffer here would have nothing to show the error with. */
            free(gp != NULL ? gp : b->p);
            free(gq != NULL ? gq : b->q);
            b->cap = 0;
            return;
        }

        b->p = gp;
        b->q = gq;
        b->cap = want;
    }

    b->p[b->n] = c;
    b->q[b->n] = (char)quoted;
    b->n++;
    b->p[b->n] = 0;
    b->q[b->n] = 0;
}

static void sb_puts(sbuf_t *b, const char *s, int quoted) {
    for (; *s != 0; s++)
        sb_putc(b, *s, quoted);
}

/* Take what is in `from` and put it in `to`, quoting every character.  Used where the text
 * cannot have been protected -- a command substitution's *output* is never protected by
 * whatever protected the `$(`, except when the `$(` itself was inside double quotes, and
 * that is decided by the caller. */
static void sb_move(sbuf_t *to, sbuf_t *from, int quoted) {
    sb_puts(to, from->p != NULL ? from->p : "", quoted);
    from->n = 0;

    if (from->p != NULL) {
        from->p[0] = 0;
        from->q[0] = 0;
    }
}

/* --- fields --------------------------------------------------------------------- */

typedef struct {
    char **v;
    int    n;
    int    cap;
} fields_t;

static int field_add(fields_t *f, const char *s) {
    if (f->n == f->cap) {
        int want = f->cap ? f->cap * 2 : 8;
        char **grown = realloc(f->v, (size_t)want * sizeof(char *));

        if (grown == NULL)
            return -1;

        f->v = grown;
        f->cap = want;
    }

    f->v[f->n] = strdup(s);

    if (f->v[f->n] == NULL)
        return -1;

    f->v[f->n + 1] = NULL;
    f->n++;
    return 0;
}

static void fields_free(fields_t *f) {
    for (int i = 0; i < f->n; i++)
        free(f->v[i]);

    free(f->v);
    memset(f, 0, sizeof(*f));
}

/* --- the default IFS ------------------------------------------------------------ */

/* ` \t\n`, and overridable because `IFS=,` is how a script reads a comma-separated line.
 * A shell that cannot be told how to split a string cannot read a CSV. */
static const char *ifs(void) {
    const char *v = var_get("IFS");

    return v != NULL ? v : " \t\n";
}

static int is_ifs(int c) {
    return c != 0 && strchr(ifs(), c) != NULL;
}

/* --- splitting and globbing ------------------------------------------------------ */

/* Split the buffer into fields on *unquoted* IFS.
 *
 * Two rules that are easy to get wrong and that every script depends on:
 *
 *   - A run of IFS characters is one separator.  `a  b` is two fields, not three.
 *   - A field that has any protected character in it is not a pattern.  `"*.c"` is a file
 *     name; `*.c` is a glob.  The check is deliberately conservative -- `"a"*` does not
 *     glob either -- because the alternative is a field that globs when the user meant a
 *     name, and a name that unexpectedly becomes several arguments.
 */
static void split_and_glob(const sbuf_t *b, fields_t *out, int always_one) {
    size_t i = 0;

    while (i <= b->n) {
        while (i < b->n && is_ifs((unsigned char)b->p[i]) && !b->q[i])
            i++;

        if (i >= b->n)
            break;

        size_t start = i;
        int    quoted = 0;

        while (i < b->n && (is_ifs((unsigned char)b->p[i]) ? b->q[i] != 0 : 1)) {
            if (b->q[i])
                quoted = 1;

            i++;
        }

        size_t len = i - start;

        /* An entirely quoted word is always one field, even when it is empty: `echo "" x`
         * prints two words, and `echo $empty x` prints one.  That difference is the whole
         * reason the "was anything quoted" flag exists. */
        if (len == 0 && !quoted)
            continue;

        char *word = malloc(len + 1);

        if (word == NULL)
            return;

        memcpy(word, b->p + start, len);
        word[len] = 0;

        if (quoted || always_one || strpbrk(word, "*?[") == NULL) {
            field_add(out, word);
            free(word);
            continue;
        }

        glob_t g;

        memset(&g, 0, sizeof(g));

        /* `GLOB_NOCHECK` is what makes `rm *.o` in a directory with no object files say
         * which name it could not find, rather than saying nothing at all. */
        if (glob(word, GLOB_NOCHECK, NULL, &g) != GLOB_NOMATCH) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                field_add(out, g.gl_pathv[k]);

            globfree(&g);
        } else {
            field_add(out, word);
        }

        free(word);
    }
}

/* --- arithmetic ----------------------------------------------------------------- */

/* A real expression evaluator would be a parser and an evaluator.  This folds over
 * + - * / % and parentheses with C's precedence, which is what shell scripts use `$(( ))`
 * for, and it is here rather than in the parser because the parser has to stay out of
 * arithmetic.
 *
 * Names are substituted before it sees them, so `$((n + 1))` arrives with `n` already a
 * number; a name that is still a name is 0, which is what every shell does and is why
 * `echo $((x))` prints 0 for an unset `x` rather than complaining. */
typedef struct {
    const char *s;
} arith_t;

static long arith_expr(arith_t *a);

static void arith_skip(arith_t *a) {
    while (*a->s == ' ')
        a->s++;
}

static long arith_atom(arith_t *a) {
    arith_skip(a);

    if (*a->s == '(') {
        a->s++;
        long v = arith_expr(a);

        arith_skip(a);

        if (*a->s == ')')
            a->s++;

        return v;
    }

    if (*a->s == '-') { a->s++; return -arith_atom(a); }
    if (*a->s == '+') { a->s++; return  arith_atom(a); }
    if (*a->s == '!') { a->s++; return !arith_atom(a); }
    if (*a->s == '~') { a->s++; return ~arith_atom(a); }

    if (isdigit((unsigned char)*a->s)) {
        char *end;
        long v = strtol(a->s, &end, 0);      /* base 0, so 0x10 is sixteen */

        a->s = end;
        return v;
    }

    if (isalpha((unsigned char)*a->s) || *a->s == '_' || *a->s == '$') {
        char   name[64];
        size_t n = 0;

        while ((isalnum((unsigned char)*a->s) || *a->s == '_') && n + 1 < sizeof(name))
            name[n++] = *a->s++;

        name[n] = 0;
        a->s += n;

        const char *v = var_get(name);

        return v != NULL ? strtol(v, NULL, 0) : 0;
    }

    return 0;
}

static long arith_term(arith_t *a) {
    long v = arith_atom(a);

    for (;;) {
        arith_skip(a);

        if (*a->s == '*') {
            a->s++;
            v *= arith_atom(a);
        } else if (*a->s == '/') {
            a->s++;
            long d = arith_atom(a);

            /* Division by zero is 0 rather than a trap: a signal raised inside `$(())`
             * would kill the shell, and a shell that dies of a typo in a loop counter is
             * worse than one that prints 0. */
            v = (d != 0) ? v / d : 0;
        } else if (*a->s == '%') {
            a->s++;
            long d = arith_atom(a);

            v = (d != 0) ? v % d : 0;
        } else {
            return v;
        }
    }
}

static long arith_expr(arith_t *a) {
    long v = arith_term(a);

    for (;;) {
        arith_skip(a);

        if (*a->s == '+') { a->s++; v += arith_term(a); }
        else if (*a->s == '-') { a->s++; v -= arith_term(a); }
        else return v;
    }
}

long sh_arith(const char *text) {
    arith_t a = { text };
    arith_skip(&a);

    /* `$(( ... ))` as well as a bare expression, because `test x -eq $((1+1))` and
     * `expr $((1+1))` are both things people write. */
    if (a.s[0] == '$' && a.s[1] == '(' && a.s[2] == '(') {
        a.s += 3;
        arith_skip(&a);

        size_t depth = 0;
        size_t j = 0;

        while (a.s[j] != 0) {
            if (a.s[j] == '(')
                depth++;
            else if (a.s[j] == ')') {
                if (depth == 0 && j + 1 < strlen(a.s) && a.s[j + 1] == ')')
                    break;

                depth--;
            }

            j++;
        }

        char *inner = malloc(j + 1);

        if (inner == NULL)
            return 0;

        memcpy(inner, a.s, j);
        inner[j] = 0;

        long v = sh_arith(inner);

        free(inner);
        return v;
    }

    return arith_expr(&a);
}

/* --- parameter expansion --------------------------------------------------------- */

/* The special parameters, which are not in the variable table.
 *
 * They read the shell's own state because that is what they *are*: `$?` is the last
 * command's status, `$#` is how many arguments this shell has, and neither is something
 * `export` could produce. */
static const char *special_param(char c) {
    static char buf[32];

    switch (c) {
    case '?':
        snprintf(buf, sizeof(buf), "%d", sh.status);
        return buf;
    case '$':
        snprintf(buf, sizeof(buf), "%ld", (long)getpid());
        return buf;
    case '#':
        snprintf(buf, sizeof(buf), "%d", sh.argc);
        return buf;
    case '0':
        return sh.argv0 != NULL ? sh.argv0 : "sh";
    case '-':
        return "";
    default:
        if (c >= '1' && c <= '9') {
            int n = c - '0';

            if (n <= sh.argc)
                return sh.argv[n];

            return NULL;
        }

        return NULL;
    }
}

static const char *lookup(const char *name) {
    if (name[1] == 0 && (isdigit((unsigned char)name[0]) || name[0] == '?' ||
                         name[0] == '$' || name[0] == '#' || name[0] == '-' ||
                         name[0] == '@' || name[0] == '*'))
        return special_param(name[0]);

    return var_get(name);
}

static void append(sbuf_t *out, const char *s, int quoted) {
    sb_puts(out, s != NULL ? s : "", quoted);
}

/* `${...}`.
 *
 * `:-` `:=` `:?` `:+` with a colon -- the colon forms also fire for an *empty* variable --
 * and `-` `=` `?` `+` without one, which do not.  That difference is the entire reason
 * both spellings exist: `${x:-none}` says "no default for an empty x" and `${x-none}`
 * says "an empty x is a value". */
static void expand_brace(sbuf_t *out, const char *body, int quoted) {
    size_t len = strlen(body);
    size_t i = 0;

    /* `${#name}` is the length, and it is answered before anything else because an unset
     * variable's length is 0 rather than an error. */
    if (len > 1 && body[0] == '#' && body[1] != '#' && body[1] != '%') {
        char name[128];
        size_t n = 0;

        i = 1;

        while (i < len && (isalnum((unsigned char)body[i]) || body[i] == '_') &&
               n + 1 < sizeof(name))
            name[n++] = body[i++];

        name[n] = 0;

        const char *v = lookup(name);
        char buf[32];

        snprintf(buf, sizeof(buf), "%zu", v != NULL ? strlen(v) : (size_t)0);
        append(out, buf, quoted);
        return;
    }

    char name[128];
    size_t n = 0;

    if (i < len && (isdigit((unsigned char)body[i]) || strchr("?$#@*-_", body[i]) != NULL)) {
        if (n + 1 < sizeof(name))
            name[n++] = body[i++];
    }

    while (i < len && (isalnum((unsigned char)body[i]) || body[i] == '_') &&
           n + 1 < sizeof(name))
        name[n++] = body[i++];

    name[n] = 0;

    const char *value = lookup(name);

    if (i >= len) {
        append(out, value, quoted);
        return;
    }

    char op = body[i++];
    int  colon = 0;

    if (op == ':' && i < len) {
        colon = 1;
        op = body[i++];
    }

    const char *arg = body + i;
    size_t      alen = len - i;
    int         unset = (value == NULL) || (colon && value[0] == 0);

    switch (op) {
    case '-':
        if (unset)
            append(out, arg, quoted);
        else
            append(out, value, quoted);

        break;

    case '+':
        if (!unset)
            append(out, arg, quoted);

        break;

    case '=':
        if (unset) {
            char *copy = malloc(alen + 1);

            if (copy != NULL) {
                memcpy(copy, arg, alen);
                copy[alen] = 0;
                var_set(name, copy, var_exported(name));
                free(copy);
            }

            append(out, arg, quoted);
        } else {
            append(out, value, quoted);
        }

        break;

    case '?':
        if (unset) {
            char msg[256];

            snprintf(msg, sizeof(msg), "%s", alen ? arg : "parameter not set");
            sh_error("%s: %s", name, msg);
            sh_exit(1);
        } else {
            append(out, value, quoted);
        }

        break;

    case '#':
    case '%': {
        /* Prefix and suffix removal, with `fnmatch` doing the matching so that
         * `${f%.*}` means what it looks like.  `#` removes a prefix, `%` a suffix, and the
         * doubled forms are greedy in the opposite direction. */
        int      greedy = alen > 0 && arg[0] == op;
        const char *pat = greedy ? arg + 1 : arg;
        const char *v = value != NULL ? value : "";
        size_t   vlen = strlen(v);

        if (op == '#') {
            for (int cut = (int)vlen; cut >= 0; cut--) {
                char *head = malloc((size_t)cut + 1);

                if (head == NULL)
                    break;

                memcpy(head, v, (size_t)cut);
                head[cut] = 0;

                int hit = fnmatch(pat, head, 0) == 0;

                free(head);

                if (hit) {
                    append(out, v + cut, quoted);
                    return;
                }

                if (!greedy)
                    break;
            }
        } else {
            for (size_t cut = 0; cut <= vlen; cut++) {
                char *tail = malloc(vlen - cut + 1);

                if (tail == NULL)
                    break;

                memcpy(tail, v + cut, vlen - cut);
                tail[vlen - cut] = 0;

                int hit = fnmatch(pat, tail, 0) == 0;

                free(tail);

                if (hit) {
                    char *head = malloc(cut + 1);

                    if (head != NULL) {
                        memcpy(head, v, cut);
                        head[cut] = 0;
                        append(out, head, quoted);
                        free(head);
                    }

                    return;
                }

                if (!greedy)
                    break;
            }
        }

        append(out, v, quoted);
        break;
    }

    default:
        append(out, value, quoted);
        break;
    }
}

/* Strip the trailing newlines from a command substitution's output.
 *
 * All of them, not just one.  A command's output ends with a newline because that is what
 * the program wrote, and `$(ls)` is a *list of names* rather than a blob with a stray byte
 * at the end -- so `for f in $(ls)` must not see an empty last field.  A program that wants
 * the newlines writes `"$(cat file)"`. */
static char *chomp(const char *s) {
    size_t n = strlen(s);

    while (n > 0 && s[n - 1] == '\n')
        n--;

    char *out = malloc(n + 1);

    if (out == NULL)
        return NULL;

    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* --- the walk --------------------------------------------------------------------- */

typedef struct {
    const char *s;
    size_t      i;
    sbuf_t     *out;
} walk_t;

/* Find the `)` that closes the `$(` at `s[i]`, counting nesting and skipping quoted runs so
 * that `$(echo "(")` does not think there is an unmatched open bracket. */
static size_t find_paren(const char *s, size_t i) {
    int    depth = 1;
    char   quote = 0;
    size_t j = i + 1;

    for (; s[j] != 0; j++) {
        char c = s[j];

        if (quote != 0) {
            if (c == '\\' && quote == '"' && s[j + 1] != 0)
                j++;
            else if (c == quote)
                quote = 0;

            continue;
        }

        if (c == '\\' && s[j + 1] != 0) {
            j++;
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }

        if (c == '(')
            depth++;
        else if (c == ')' && --depth == 0)
            return j;
    }

    return strlen(s);
}

static void walk(walk_t *w, int quoted);

static void do_dollar(walk_t *w, int quoted) {
    const char *s = w->s;
    size_t      i = w->i + 1;

    if (s[i] == 0) {
        /* A trailing `$` is a literal `$`: `echo $` prints a dollar sign, and a printf
         * format that ends in one has to survive. */
        sb_putc(w->out, '$', quoted);
        w->i = i;
        return;
    }

    char c = s[i];

    if (c == '(') {
        /* `$((...))` is arithmetic and `$(...)` is a command.  The two look alike and are
         * distinguished by the second `(`, which is why this check is first. */
        if (s[i + 1] == '(') {
            size_t j = i + 2;
            int    depth = 0;

            while (s[j] != 0) {
                if (s[j] == '(')
                    depth++;
                else if (s[j] == ')') {
                    if (depth == 0 && s[j + 1] == ')')
                        break;

                    depth--;
                }

                j++;
            }

            char *inner = malloc(j - i - 1);
            char  buf[32];

            if (inner == NULL)
                return;

            memcpy(inner, s + i + 2, j - i - 2);
            inner[j - i - 2] = 0;

            snprintf(buf, sizeof(buf), "%ld", sh_arith(inner));
            append(w->out, buf, quoted);
            free(inner);
            w->i = j + 2;
            return;
        }

        size_t end = find_paren(s, i);
        char  *inner = malloc(end - i);

        if (inner == NULL)
            return;

        memcpy(inner, s + i + 1, end - i - 1);
        inner[end - i - 1] = 0;

        char *captured = sh_run_capture(inner);

        if (captured != NULL) {
            char *trimmed = chomp(captured);

            append(w->out, trimmed != NULL ? trimmed : "", quoted);
            free(trimmed);
            free(captured);
        }

        free(inner);
        w->i = end + 1;
        return;
    }

    if (c == '{') {
        size_t j = i + 1;
        int    depth = 1;

        while (s[j] != 0 && depth > 0) {
            if (s[j] == '{')
                depth++;
            else if (s[j] == '}')
                depth--;

            if (depth > 0)
                j++;
        }

        char *body = malloc(j - i);

        if (body == NULL)
            return;

        memcpy(body, s + i + 1, j - i - 1);
        body[j - i - 1] = 0;

        expand_brace(w->out, body, quoted);
        free(body);
        w->i = j + 1;
        return;
    }

    if (c == '@' || c == '*') {
        /* `$*` joins the parameters with the first character of IFS; `$@` is the same
         * thing after splitting, because the separator is unquoted IFS and splitting breaks
         * at it.  The one place they differ is `"$@"`, which `expand_common()` handles
         * before it gets here, because there the separator must *not* be split on. */
        for (int k = 0; k < sh.argc; k++) {
            if (k > 0)
                sb_putc(w->out, ifs()[0], quoted);

            append(w->out, sh.argv[k], quoted);
        }

        w->i = i + 1;
        return;
    }

    if (isdigit((unsigned char)c) || c == '?' || c == '$' || c == '#' || c == '-') {
        const char *v = special_param(c);

        append(w->out, v, quoted);
        w->i = i + 1;
        return;
    }

    {
        size_t j = i;
        char   name[128];
        size_t n = 0;

        while (j < strlen(s) && (isalnum((unsigned char)s[j]) || s[j] == '_') &&
               n + 1 < sizeof(name))
            name[n++] = s[j++];

        if (n == 0) {
            sb_putc(w->out, '$', quoted);
            w->i = i + 1;
            return;
        }

        name[n] = 0;
        append(w->out, var_get(name), quoted);
        w->i = j;
    }
}

/* One pass over the word, producing text and marking what was protected. */
static void walk(walk_t *w, int quoted) {
    const char *s = w->s;

    while (s[w->i] != 0) {
        char c = s[w->i];

        if (c == '\\') {
            if (s[w->i + 1] != 0 && (!quoted || strchr("$`\"\\\n", s[w->i + 1]) != NULL)) {
                /* Inside double quotes a backslash only protects `$ `, a backtick, a
                 * double quote, a backslash and a newline.  Anywhere else it is a literal
                 * backslash, which is what a Windows path needs. */
                sb_putc(w->out, s[w->i + 1], 1);
                w->i += 2;
                continue;
            }

            sb_putc(w->out, c, 0);
            w->i++;
            continue;
        }

        if (c == '\'') {
            /* Everything to the next quote is literal: no `$`, no glob, no escape.  This
             * is the quoting that exists so somebody else's shell code can be passed
             * through untouched, and it cannot nest. */
            size_t j = w->i + 1;

            while (s[j] != 0 && s[j] != '\'')
                j++;

            for (size_t k = w->i + 1; k < j; k++)
                sb_putc(w->out, s[k], 1);

            w->i = (s[j] == 0) ? j : j + 1;
            continue;
        }

        if (c == '"') {
            size_t j = w->i + 1;

            while (s[j] != 0 && s[j] != '"') {
                if (s[j] == '\\' && s[j + 1] != 0 &&
                    strchr("$`\"\\", s[j + 1]) != NULL) {
                    sb_putc(w->out, s[j + 1], 1);
                    j += 2;
                    continue;
                }

                if (s[j] == '$' || s[j] == '`') {
                    /* Expanded inside the quotes, with everything it produces marked as
                     * protected -- which is what makes `"$(ls)"` one long string rather
                     * than a list of names. */
                    size_t start = j;

                    if (s[j] == '`' || (s[j] == '$' && s[j + 1] == '(')) {
                        char open = s[j] == '`' ? '`' : '(';
                        char close = s[j] == '`' ? '`' : ')';

                        if (open == '(')
                            j = find_paren(s, j + 1);
                        else
                            while (s[j + 1] != 0 && s[j + 1] != close)
                                j++;
                    } else if (s[j] == '$' && s[j + 1] == '{') {
                        int depth = 1;

                        j += 2;

                        while (s[j] != 0 && depth > 0) {
                            if (s[j] == '{')
                                depth++;
                            else if (s[j] == '}')
                                depth--;

                            j++;
                        }
                    } else {
                        while (s[j + 1] != 0 &&
                               (isalnum((unsigned char)s[j + 1]) || s[j + 1] == '_'))
                            j++;
                    }

                    if (j == start)
                        j++;

                    char *piece = malloc(j - start + 1);

                    if (piece != NULL) {
                        memcpy(piece, s + start, j - start);
                        piece[j - start] = 0;

                        walk_t sub = { piece, 0, w->out };

                        walk(&sub, 1);
                        free(piece);
                    }

                    continue;
                }

                sb_putc(w->out, s[j], 1);
                j++;
            }

            w->i = (s[j] == 0) ? j : j + 1;
            continue;
        }

        if (c == '$') {
            do_dollar(w, quoted);
            continue;
        }

        if (c == '`' && !quoted) {
            size_t j = w->i + 1;

            while (s[j] != 0 && s[j] != '`')
                j++;

            char *inner = malloc(j - w->i);

            if (inner != NULL) {
                memcpy(inner, s + w->i + 1, j - w->i - 1);
                inner[j - w->i - 1] = 0;

                char *captured = sh_run_capture(inner);

                if (captured != NULL) {
                    char *trimmed = chomp(captured);

                    append(w->out, trimmed != NULL ? trimmed : "", quoted);
                    free(trimmed);
                    free(captured);
                }

                free(inner);
            }

            w->i = j + 1;
            continue;
        }

        sb_putc(w->out, c, quoted);
        w->i++;
    }
}

/* --- the entry points -------------------------------------------------------------- */

/* The one case `"$@"` has to be special: each positional parameter is its own field, with
 * no splitting and no joining, so that `set -- "$@"` and `for f in "$@"` both handle an
 * argument containing spaces.  Everything else goes through the normal path, where `$@` and
 * `$*` differ only in whether the separator gets split on -- which is the same answer, for
 * the same reason, without a second implementation. */
static int expand_common(const char *word, char ***out, int single_field) {
    fields_t result;
    sbuf_t   text;
    int      tilde = 0;

    memset(&result, 0, sizeof(result));
    sb_init(&text);

    if (word[0] == '~') {
        const char *home = NULL;
        size_t      n = 1;

        while (word[n] != 0 && word[n] != '/')
            n++;

        if (n == 1) {
            home = var_get("HOME");
        } else {
            char name[64];
            size_t len = n - 1 < sizeof(name) - 1 ? n - 1 : sizeof(name) - 1;

            memcpy(name, word + 1, len);
            name[len] = 0;

            /* There is one user, so `~root` is `/` and `~anyone` names nobody.  Inventing
             * a home for an unknown user would be wrong in a way a person would have to
             * debug. */
            home = strcmp(name, "root") == 0 ? "/" : NULL;
        }

        if (home != NULL) {
            sb_puts(&text, home, 1);
            tilde = 1;
        }
    }

    walk_t w = { word, tilde, &text };

    walk(&w, tilde);

    if (single_field) {
        char **one = malloc(sizeof(char *));

        if (one == NULL) {
            sb_free(&text);
            return -1;
        }

        one[0] = strdup(text.p != NULL ? text.p : "");
        one[1] = NULL;
        sb_free(&text);
        *out = one;
        return 1;
    }

    if (strcmp(word, "\"$@\"") == 0) {
        for (int k = 0; k < sh.argc; k++)
            field_add(&result, sh.argv[k]);
    } else {
        split_and_glob(&text, &result, 0);
    }

    sb_free(&text);

    *out = result.v;
    return result.n;
}

int expand_word(const char *word, char ***out) {
    return expand_common(word, out, 0);
}

int expand_assign(const char *word, char **out) {
    const char *eq = strchr(word, '=');

    if (eq == NULL) {
        *out = strdup("");
        return 0;
    }

    /* Only the part after the `=` is expanded.  Expanding the whole word and chopping the
     * name off afterwards would work until a value began with the name, and `x=x` is
     * exactly that. */
    char **one = NULL;

    expand_common(eq + 1, &one, 1);

    /* The string is *moved* to the caller: not copied, and not freed.
     *
     * It used to be freed here as well as handed over, on the reasoning that `one` was a
     * temporary.  It is not a temporary: `*out` is that very pointer, so the caller got a
     * block that was already back on the allocator's free list, and its own `free()` --
     * the `free(value)` at the bottom of the assignment loop in `expand_command()` -- was
     * the second release of the same block.
     *
     * That is neither a leak nor an immediate crash: a block freed twice that happens to
     * be larger than the block being freed walks the free list into a ring of one, and the
     * next allocation or free above it spins for ever.  What it looked like was
     * `sh script.sh` hanging on the first `x=42` in the script -- and the report blaming
     * the *kernel*, because the shell's `waitpid` never returned and the task list said a
     * task was blocked in `wait`.  It took the task list, the script the shell had been
     * given, the allocator's disassembly and a print of the offending block to arrive at a
     * function in the shell.
     *
     * The array is still ours to free: `expand_common()` allocated it and nothing after
     * this line can reach it. */
    *out = (one != NULL && one[0] != NULL) ? one[0] : strdup("");

    free(one);

    return 0;
}

char *expand_to_string(const char *word) {
    fields_t f;
    sbuf_t   b;

    memset(&f, 0, sizeof(f));
    sb_init(&b);

    walk_t w = { word, 0, &b };

    walk(&w, 0);
    split_and_glob(&b, &f, 1);

    for (int i = 0; i < f.n; i++) {
        if (i > 0)
            sb_putc(&b, ' ', 0);

        sb_puts(&b, f.v[i], 0);
    }

    fields_free(&f);

    return b.p != NULL ? b.p : strdup("");
}
