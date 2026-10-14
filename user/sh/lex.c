/* minx - the lexer.
 *
 * Text into tokens, and the only thing it knows about quoting is that a quote makes the
 * next metacharacter an ordinary character.  The marks themselves are *kept* in the word:
 * a shell has to know at expansion time whether `$x` was quoted, and a token that has
 * already thrown the quotes away cannot answer that.
 *
 * That decision is why this file does not build a list of (text, was-quoted) pieces per
 * word.  Keeping the marks costs nothing -- they are characters the shell was given -- and
 * it moves the whole of quoting into the expander, where the rules actually are.
 *
 * Two things a shell's lexer has to get right that a C one does not have to think about:
 *
 *   - An operator is only an operator in a *word's* position.  `a>b` is two things and
 *     `echo a>b` is three, while `echo "a>b"` is one.  So the loop that reads a word stops
 *     at a metacharacter, and whether that metacharacter is an operator depends only on
 *     where the word ended.
 *
 *   - `$(` and `` ` `` are words, not operators, because what is inside them is text that
 *     has to be parsed later and run in another process.  The lexer records the opening
 *     and hands the parser a token whose text is the *inside*, with matching nesting found
 *     here -- counting nesting is lexical and counting it later would mean the parser
 *     re-reading raw text.
 */
#include "sh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The operators, longest first: `>>` before `>` or the lexer would split `>>` into two `>`s
 * and the second would start a file named ">" . */
static const struct {
    const char *text;
    tok_kind_t  kind;
    redir_kind_t rkind;
} operators[] = {
    { ";;",  T_DSEMI,  RD_OUT },
    { "&&",  T_AND_IF, RD_OUT },
    { "||",  T_OR_IF,  RD_OUT },
    { "<<",  T_REDIR,  RD_IN  },   /* here-documents are not implemented; see below */
    { ">>",  T_REDIR,  RD_APPEND },
    { "<&",  T_REDIR,  RD_IN2 },
    { ">&",  T_REDIR,  RD_OUT2FD },
    { ">|",  T_REDIR,  RD_OUT2 },
    { "&>",  T_REDIR,  RD_ALLIN },
    { "<",   T_REDIR,  RD_IN },
    { ">",   T_REDIR,  RD_OUT },
    { "|",   T_PIPE,   RD_OUT },
    { "&",   T_SEMI,   RD_OUT },
    { ";",   T_SEMI,   RD_OUT },
    { "(",   T_LPAREN, RD_OUT },
    { ")",   T_RPAREN, RD_OUT },
    { "{",   T_LBRACE, RD_OUT },
    { "}",   T_RBRACE, RD_OUT },
};

/* Words that are reserved in some positions and ordinary in others.  The lexer tags them
 * and the parser decides: `while` is a keyword at the start of a command and the name of a
 * program after it. */
static const struct {
    const char *text;
    tok_kind_t  kind;
} reserved[] = {
    { "if",    T_IF },
    { "then",  T_THEN },
    { "elif",  T_ELIF },
    { "else",  T_ELSE },
    { "fi",    T_FI },
    { "for",   T_FOR },
    { "in",    T_IN },
    { "do",    T_DO },
    { "done",  T_DONE },
    { "while", T_WHILE },
    { "until", T_UNTIL },
    { "case",  T_CASE },
    { "esac",  T_ESAC },
};

/* A growable string, because the lexer cannot know how long a word is before it has read
 * it and `realloc` per character is a malloc per character. */
typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} buf_t;

static void buf_put(buf_t *b, char c) {
    if (b->n + 2 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 32;
        char  *grown = realloc(b->p, want);

        if (grown == NULL) {
            /* Out of memory in the lexer of a shell on a machine with 512 MiB.  Returning
             * what has been read is better than dying: the caller sees a truncated line and
             * reports a syntax error, which is a thing a person can act on. */
            return;
        }

        b->p = grown;
        b->cap = want;
    }

    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void buf_reset(buf_t *b) {
    b->n = 0;

    if (b->p != NULL)
        b->p[0] = 0;
}

static int push_token(tokens_t *t, tok_kind_t kind, const char *text, size_t len) {
    if (t->n == t->cap) {
        int want = t->cap ? t->cap * 2 : 32;
        token_t *grown = realloc(t->v, (size_t)want * sizeof(token_t));

        if (grown == NULL)
            return -1;

        t->v = grown;
        t->cap = want;
    }

    token_t *tok = &t->v[t->n++];

    memset(tok, 0, sizeof(*tok));
    tok->kind = kind;

    if (text != NULL && len > 0) {
        tok->text = malloc(len + 1);

        if (tok->text == NULL)
            return -1;

        memcpy(tok->text, text, len);
        tok->text[len] = 0;
    }

    return 0;
}

/* Is `name` a legal variable name?
 *
 * Checked rather than assumed, because `a-b=1` is a command called `a-b` with the
 * argument `=1` on every Unix, and treating it as an assignment would make `echo a-b=1`
 * print nothing. */
static int is_assignment(const char *s, size_t len) {
    if (len == 0 || !(*s == '_' || isalpha((unsigned char)*s)))
        return 0;

    for (size_t i = 1; i < len; i++) {
        if (s[i] == '=')
            return 1;                   /* must be the first `=` and the name before it */

        if (!(s[i] == '_' || isalnum((unsigned char)s[i])))
            return 0;
    }

    return 0;
}

/* Find the end of a `$(` or a backtick, respecting nesting and quotes.
 *
 * Returns the index just past the closing bracket, or -1 if the line ends inside it.  The
 * nesting count is over `(`/`)` only when they are *inside* the construct and not inside a
 * quoted string -- which is why the quote state is tracked rather than the count alone.
 * `$(echo "(")` has one open paren inside a quoted string and no unmatched close, and a
 * counter that did not know about quotes would look for one that is not there. */
static long find_close(const char *s, size_t n, size_t from, char open, char close) {
    int    depth = 1;
    char   quote = 0;

    /* A `case` body has parentheses that do not nest.
     *
     * `$(case y in a) echo a;; esac)` is legal and the `)` after `a` is a pattern
     * terminator, not the end of the substitution -- but a depth count reads it as the end,
     * so the substitution closed after `a` and the rest of the line was left for the parser
     * as a `;; esac )` it could not use.  bash accepts it, and it is not rare: wrapping a
     * `case` in `$( )` is how a shell script returns a value from one.
     *
     * So a `case` suppresses the count until its `esac`.  The words are matched as whole
     * words so that `casey` and `esac_count` are not mistaken for the keywords, and a
     * `case` with no `esac` simply runs to the end and leaves the substitution unterminated,
     * which the caller reports. */
    int in_case = 0;
    int at_word_start = 1;

    for (size_t i = from; i < n; i++) {
        char c = s[i];

        if (quote != 0) {
            if (c == '\\' && quote == '"')
                i++;
            else if (c == quote)
                quote = 0;

            at_word_start = 0;
            continue;
        }

        if (c == '\\') {
            i++;
            at_word_start = 0;
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            at_word_start = 0;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' ||
            c == '(' || c == ')' || c == '<' || c == '>') {
            at_word_start = 1;
        } else if (at_word_start) {
            size_t len = strcspn(s + i, " \t\n;&|()<>");

            if (len == 4 && memcmp(s + i, "case", 4) == 0)
                in_case = 1;
            else if (len == 4 && memcmp(s + i, "esac", 4) == 0)
                in_case = 0;

            at_word_start = 0;
            i += len - 1;
            continue;
        }

        if (in_case)
            continue;

        if (c == open)
            depth++;
        else if (c == close && --depth == 0)
            return (long)(i + 1);
    }

    return -1;
}

/* Read one word, stopping at a metacharacter that is not quoted.
 *
 * The marks are kept.  `\` before a metacharacter removes the backslash's *effect* but
 * keeps the pair, so that `\ ` is one character that is a space rather than two that are
 * a backslash and a space -- which is what the expander needs to see. */
static long lex_word(const char *s, size_t n, size_t i, buf_t *out) {
    buf_reset(out);

    for (; i < n; i++) {
        char c = s[i];

        if (c == '\\') {
            if (i + 1 < n) {
                buf_put(out, c);
                buf_put(out, s[++i]);
                continue;
            }

            buf_put(out, c);
            continue;
        }

        if (c == '\'' || c == '"') {
            buf_put(out, c);

            /* Copy to the matching quote, so that a `;` inside quotes is not an operator
             * and a `$(` inside quotes is not a command substitution. */
            char quote = c;

            for (i++; i < n; i++) {
                buf_put(out, s[i]);

                if (s[i] == '\\' && quote == '"' && i + 1 < n) {
                    buf_put(out, s[++i]);
                    continue;
                }

                if (s[i] == quote)
                    break;
            }

            continue;
        }

        /* `$(` and `$((` are *not* a metacharacter boundary, and treating them as one is
         * the whole difference between `echo $(date)` working and `echo $(date)` lexing as
         * three tokens.
         *
         * `(` ends a word -- it is the grouping operator -- but the `(` in `$(` belongs to
         * the substitution, and the `(` in `$((` is arithmetic.  Both are one piece of the
         * word as the user typed it, and both are expanded by the *expansion* pass, which
         * is the pass that owns quoting and knows what `$( )` and `$(( ))` mean.  Splitting
         * them here handed expansion a word truncated at the `$`: `n=$((n + 1))` became
         * `n=$`, `(`, `(`, `n`, `+`, `1`, `)`, `)`, and the assignment was left with no
         * value while the parser tried to make sense of five stray operators.
         *
         * So the group is copied through, whole, with the same quote-aware depth counting
         * `find_close()` uses -- which also gets `$((1 + (2)))` right, because the inner
         * `(` raises the depth and the first `)` at depth two does not close it. */
        if (c == '$' && i + 1 < n && s[i + 1] == '(') {
            /* From `i + 2`, which is *past* the `(`.  `find_close()` counts the opening
             * parenthesis as a depth of one already, so starting it on the `(` itself
             * counts that parenthesis twice -- and `$((n + 1))` then never closes, the
             * scan runs to the end of the line, and the word is truncated at the `$`.
             */
            long end = find_close(s, n, i + 2, '(', ')');

            if (end < 0) {
                /* Unterminated: the word ends here and the parser reports it, which is a
                 * better answer than a silently truncated substitution. */
                break;
            }

            for (size_t k = i; k < (size_t)end; k++)
                buf_put(out, s[k]);

            i = (size_t)end - 1;
            continue;
        }

        /* Unquoted metacharacters end the word. */
        if (c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' || c == '(' ||
            c == ')' || c == '<' || c == '>' || c == '`' || c == '\n')
            break;

        buf_put(out, c);
    }

    return (long)i;
}

int lex_line(const char *line, tokens_t *out) {
    size_t n = strlen(line);
    size_t i = 0;
    buf_t  b = { NULL, 0, 0 };

    memset(out, 0, sizeof(*out));

    for (;;) {
        /* Blanks.  A newline is a token, because it ends a command. */
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            i++;

        if (i >= n)
            break;

        if (line[i] == '\n') {
            i++;

            /* Consecutive newlines are one separator.  Without this a blank line in a
             * script produces an empty command, and the parser reports a syntax error for
             * a line a person can see is blank. */
            size_t j = i;

            while (j < n && (line[j] == ' ' || line[j] == '\t'))
                j++;

            if (j >= n || line[j] == '\n')
                continue;

            if (push_token(out, T_NEWLINE, NULL, 0) != 0)
                goto oom;

            continue;
        }

        /* A comment, which runs to the end of the line and is *not* a newline: `ls # x` is
         * one command, and `ls # x` followed by another line is two. */
        if (line[i] == '#') {
            while (i < n && line[i] != '\n')
                i++;

            continue;
        }

        /* A `$(` or `$((` at the *start* of a word.  `lex_word()` keeps these inside a
         * word already, so this only runs for a word that is nothing but a substitution --
         * `echo $(date)`, where the `$(` follows a blank rather than other word
         * characters.
         *
         * The two forms differ in where their contents start and end, and the difference
         * is what tells `parse_simple()` which spelling to put back together: `$(ls)` holds
         * `ls`, `$((n + 1))` holds `(n + 1)` -- the outer pair was taken as the operator
         * and the inner pair is part of the expression.  Getting that wrong made every
         * `$(( ... ))` expand as a command substitution of the expression, which is how
         * `n=$((n + 1))` became a run of a program called `n + 1`. */
        if (line[i] == '$' && i + 1 < n && line[i + 1] == '(') {
            /* Both forms take their contents from just inside the first `(`, and
             * `find_close()` is told to start there with a depth of one -- which is the
             * count for the `(` at `i + 1`.
             *
             * The two then differ without any extra code.  For `$(ls)` the contents are
             * `ls`: the `)` that ends it closes the first paren and `find_close()` stops.
             * For `$((n + 1))` the contents are `(n + 1)`: the first `)` only brings the
             * depth back to one and the scan continues to the second, so the token's text
             * still begins with the `(`.  That leading `(` is the whole of the distinction,
             * and `parse_simple()` reads it back to choose the spelling. */
            long end = find_close(line, n, i + 2, '(', ')');

            if (end < 0) {
                free(b.p);
                return -1;              /* unterminated */
            }

            if (push_token(out, T_DPAREN, line + i + 2, (size_t)(end - i - 3)) != 0)
                goto oom;

            i = (size_t)end;
            continue;
        }

        if (line[i] == '`') {
            long end = find_close(line, n, i + 1, '(', ')');

            if (end < 0) {
                free(b.p);
                return -1;
            }

            if (push_token(out, T_BACKTICK, line + i + 1, (size_t)(end - i - 2)) != 0)
                goto oom;

            i = (size_t)end;
            continue;
        }

        if (isdigit((unsigned char)line[i]) && i + 1 < n &&
            (line[i + 1] == '>' || line[i + 1] == '<')) {
            int fd = line[i] - '0';
            size_t j = i + 1;
            redir_kind_t kind;
            char op[3] = { 0, 0, 0 };

            /* The longest operator that starts here. */
            if (j + 1 < n && line[j + 1] == '>') {
                op[1] = '>';
                kind = (fd == 2) ? RD_ERRAPPEND : RD_APPEND;
            } else if (j + 1 < n && line[j + 1] == '&') {
                op[1] = '&';
                kind = (fd == 2) ? RD_ERRTOFD : RD_OUT2FD;
            } else {
                op[1] = line[j];
                kind = (fd == 2) ? RD_ERR : RD_IN;
            }

            (void)op;

            if (push_token(out, T_REDIR, NULL, 0) != 0)
                goto oom;

            out->v[out->n - 1].rkind = kind;
            /* The descriptor is prepended to the next word, which is how the expander and
             * the executor both learn which descriptor this operator is about -- and why
             * `2>&1` can be told from `>&2` without a table of two-character operators. */
            {
                char num[4];

                num[0] = (char)('0' + fd);
                num[1] = 0;

                if (push_token(out, T_WORD, num, 1) != 0)
                    goto oom;

                /* Marked, because a parser cannot tell this from the argument of a `>&`
                 * on its own -- both are a `T_WORD` of one digit -- and guessing made
                 * `>&2` into a redirection with no argument.  See
                 * `token_t.fd_prefixed`. */
                out->v[out->n - 1].fd_prefixed = 1;
            }

            /* Past the *whole* operator, which is three characters for `2>&` and two for
             * everything else.  It was `j + 1` for all of them, which left the `&` of
             * `2>&1` in the input for the next pass: that pass read it as the background
             * operator, so `{ ...; } 2>&1` parsed as a background command whose argument was
             * the word `1`.
             *
             * Nothing said so.  `2>&1` is the redirection that appears most often in every
             * shell script ever written, and it was quietly a different operator -- so the
             * test script's `>&2` check reported a *missing line* while the descriptor
             * plumbing it was written to test was never reached at all. */
            i = (line[j + 1] == '&') ? j + 2 : j + 1;
            continue;
        }

        /* An operator. */
        {
            int matched = 0;

            for (size_t k = 0; k < sizeof(operators) / sizeof(operators[0]); k++) {
                size_t len = strlen(operators[k].text);

                if (i + len <= n && memcmp(line + i, operators[k].text, len) == 0) {
                    if (operators[k].kind == T_SEMI)
                        out->n == 0 ? (void)0 : (void)0;

                    if (push_token(out, operators[k].kind, NULL, 0) != 0)
                        goto oom;

                    out->v[out->n - 1].rkind = operators[k].rkind;

                    if (operators[k].text[0] == '&' && len == 1)
                        out->v[out->n - 1].background = 1;

                    i += len;
                    matched = 1;
                    break;
                }
            }

            if (matched)
                continue;
        }

        /* A word. */
        {
            long end = lex_word(line, n, i, &b);

            if (end == (long)i) {
                /* No progress: a character that is neither a blank nor an operator nor
                 * anything `lex_word` accepts.  Treating it as a one-character word is
                 * better than looping for ever. */
                buf_put(&b, line[i]);
                i++;
            } else {
                i = (size_t)end;
            }

            /* A reserved word only where one can begin a command.  Anywhere else it is a
             * name, which is what makes `echo if` print "if" and `if true; then echo > if;
             * fi` create a file called `if`.
             *
             * "Where one can begin a command" is a question about the token immediately
             * before this one and nothing else.  It used to be answered by walking the token
             * list from the beginning and stopping at the first token that was not a
             * separator -- which answers a different question, "is this the first word on
             * the line", and made every keyword after the first an ordinary word.  `if true;
             * then echo; fi` lexed as `if` (a keyword) then `then`, `else` and `fi` (words),
             * so every compound construct failed to parse with one unhelpful sentence that
             * named neither the construct nor the token. */
            tok_kind_t kind = T_WORD;
            int        at_start = 1;

            if (out->n > 0) {
                tok_kind_t prev = out->v[out->n - 1].kind;

                /* Every token after which a *command* may begin, so a reserved word
                 * there is a keyword.
                 *
                 * The separators are the obvious half and the keyword tokens are the half
                 * that is easy to miss: after `do`, `then`, `else` or `elif` the next word
                 * starts a command, so `do if C; then A; fi` has to lex that `if` as a
                 * keyword.  With only the separators listed it came out a `T_WORD`, the
                 * parser read it as a program called `if`, and the construct was reported as
                 * "`done' expected" about a line that was correct.
                 *
                 * So it is not "a separator" at all: it is "a token that cannot be part of
                 * the command before it".  A word is, which is why `echo if` prints `if`.
                 *
                 * `T_REDIR` is deliberately *not* in the list, which reverses what the
                 * comment above used to claim, and bash settles it: in `echo hi > if` the
                 * word after the redirection is a file name, and in `> if true` it is still a
                 * file name.  The old comment said a redirection does not end the region, so
                 * that `> if true` would see `if` as a keyword -- and running that in bash
                 * creates a file called `if`.  A shell that turned a redirection's target
                 * into a keyword would make `echo hi > if` write to nothing at all. */
                at_start = prev == T_SEMI || prev == T_NEWLINE || prev == T_AND_IF ||
                           prev == T_OR_IF || prev == T_PIPE || prev == T_LPAREN ||
                           prev == T_LBRACE ||
                           prev == T_IF || prev == T_THEN || prev == T_ELIF ||
                           prev == T_ELSE || prev == T_FI || prev == T_FOR ||
                           prev == T_IN || prev == T_DO || prev == T_DONE ||
                           prev == T_WHILE || prev == T_UNTIL || prev == T_CASE ||
                           prev == T_ESAC || prev == T_RPAREN || prev == T_RBRACE;

                /* After `;;` a reserved word is a keyword even though nothing separates it
                 * from the operator, and `case x in a) y;; esac` is the one place that
                 * happens.  `esac` came out a `T_WORD` and every `case` in the test script
                 * was reported as "a case arm must be patterns followed by `)'" -- a
                 * message about the *arm*, for a failure that was the word after it. */
                if (prev == T_DSEMI)
                    at_start = 1;

                /* After the `;` that closes a construct -- `if C; then A; fi; echo B` --
                 * and after `do`, `then` and `else`, a reserved word is back to being a
                 * name.  This is not new; it is what `at_start` already means, and it is
                 * why `echo fi` prints "fi".  What is worth saying is that it also means a
                 * keyword may be a keyword in one position and a word in another *within the
                 * same line*, and the parser relies on that for `fi` after `; fi`: the `;`
                 * there ends nothing, and `echo` after it is an ordinary command. */
            }

            /* `in` is the one reserved word that does not begin a command: it follows
             * the loop variable in a `for` header, which is a word, so the rule above
             * would leave it a word and `for x in ...` would not parse.  Tagging it
             * always costs a program named `in` its name, which is a smaller price than
             * every `for` loop failing. */
            if (at_start || strcmp(b.p != NULL ? b.p : "", "in") == 0) {
                for (size_t k = 0; k < sizeof(reserved) / sizeof(reserved[0]); k++) {
                    if (strcmp(b.p, reserved[k].text) == 0) {
                        kind = reserved[k].kind;
                        break;
                    }
                }
            }

            if (kind == T_WORD && at_start && is_assignment(b.p, strlen(b.p)))
                kind = T_ASSIGN;

            if (push_token(out, kind, b.p, b.n) != 0)
                goto oom;
        }
    }

    free(b.p);

    return push_token(out, T_EOF, NULL, 0);

oom:
    free(b.p);
    return -1;
}

void tokens_free(tokens_t *t) {
    for (int i = 0; i < t->n; i++)
        free(t->v[i].text);

    free(t->v);
    memset(t, 0, sizeof(*t));
}
