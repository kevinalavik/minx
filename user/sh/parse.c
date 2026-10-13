/* minx - the parser.
 *
 * Tokens into a tree, by recursive descent, and it knows nothing about variables, files or
 * processes.  Every decision it makes is about grammar, which is why it can be read on its
 * own: `if` is here because `if` is a keyword, not because `if` runs something.
 *
 * The precedence chain, which is the whole of the design:
 *
 *   program  := linebreak (complete_command linebreak)*
 *   and_or   := pipeline ( ('&&' | '||') linebreak pipeline )*
 *   pipeline := command ( '|' linebreak command )*
 *   command  := simple_command | compound_command [redirects] | function_definition
 *
 * Each of those is one function, each calls the one below it, and `&&` binds more loosely
 * than `|` because `and_or` calls `pipeline` and not the other way round.  That ordering is
 * the entire meaning of "binds more loosely", and it is why `a && b | c` is `a && (b | c)`
 * and not `(a && b) | c` -- which is the difference between running `c` once and running
 * it when `a` fails.
 *
 * Errors carry a message rather than a code, because the only thing that will ever print
 * one is a person who typed something wrong.
 */
#include "sh.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    tokens_t     *t;
    int           at;
    const char   *err;
} parser_t;

/* A few words, bounded.  A command line longer than this is refused with a message rather
 * than silently truncated, which is the one thing a parser must not do. */
#define MAX_ARGS   512
#define MAX_STAGES 64
#define MAX_ARMS   128

static cmd_t *parse_and_or(parser_t *p);

static token_t *peek(parser_t *p) {
    return &p->t->v[p->at];
}

static token_t *next(parser_t *p) {
    token_t *t = &p->t->v[p->at];

    if (t->kind != T_EOF)
        p->at++;

    return t;
}

static int at(parser_t *p, tok_kind_t k) {
    return p->t->v[p->at].kind == k;
}

static int accept(parser_t *p, tok_kind_t k) {
    if (!at(p, k))
        return 0;

    p->at++;
    return 1;
}

static void fail(parser_t *p, const char *what) {
    if (p->err == NULL)
        p->err = what;
}

static void failf(parser_t *p, const char *what, const char *got) {
    static char buf[128];

    if (p->err != NULL)
        return;

    snprintf(buf, sizeof(buf), "%s (got `%s')", what, got != NULL ? got : "?");
    p->err = buf;
}

static cmd_t *node(cmd_kind_t kind) {
    cmd_t *c = calloc(1, sizeof(*c));

    if (c != NULL)
        c->kind = kind;

    return c;
}

void cmd_free(cmd_t *c) {
    if (c == NULL)
        return;

    /* Recursive, and that is fine: the tree's depth is the nesting depth of the input,
     * which for anything a person types is under twenty. */
    for (int i = 0; i < c->nwords; i++)
        free(c->words[i]);

    for (int i = 0; i < c->nassigns; i++)
        free(c->assigns[i]);

    for (redir_t *r = c->redirs; r != NULL;) {
        redir_t *next = r->next;

        free(r->word);
        free(r);
        r = next;
    }

    for (int i = 0; i < c->nstages; i++)
        cmd_free(c->stages[i]);

    free(c->stages);

    for (int i = 0; i < c->narms; i++) {
        for (int k = 0; c->arms[i].patterns != NULL && c->arms[i].patterns[k] != NULL; k++)
            free(c->arms[i].patterns[k]);

        free(c->arms[i].patterns);
        cmd_free(c->arms[i].body);
    }

    free(c->arms);
    free(c->name);
    free(c->words);
    free(c->assigns);
    free(c->stages);
    free(c->arms);
    cmd_free(c->a);
    cmd_free(c->b);
    free(c);
}

/* Linebreaks, which are separators and not errors.
 *
 * A `;` and a newline are the same thing to this parser, and treating them differently is
 * how `if true; then` fails and `if true\nthen` works. */
static void skip_separators(parser_t *p) {
    while (at(p, T_NEWLINE) || at(p, T_SEMI))
        p->at++;
}

static int add_word(cmd_t *c, const char *text) {
    char **grown = realloc(c->words, (size_t)(c->nwords + 2) * sizeof(char *));

    if (grown == NULL)
        return -1;

    c->words = grown;
    c->words[c->nwords] = strdup(text != NULL ? text : "");
    c->words[c->nwords + 1] = NULL;
    c->nwords++;
    return 0;
}

static int add_assign(cmd_t *c, const char *text) {
    char **grown = realloc(c->assigns, (size_t)(c->nassigns + 2) * sizeof(char *));

    if (grown == NULL)
        return -1;

    c->assigns = grown;
    c->assigns[c->nassigns] = strdup(text != NULL ? text : "");
    c->nassigns++;
    return 0;
}

static int add_redir(cmd_t *c, redir_kind_t kind, const char *word) {
    redir_t *r = calloc(1, sizeof(*r));

    if (r == NULL)
        return -1;

    r->kind = kind;
    r->word = strdup(word != NULL ? word : "");

    /* Appended, so that redirections are applied left to right -- which is what makes
     * `> a > b` mean "the second wins" and not "the first wins", and which is the order
     * they were written in. */
    redir_t **slot = &c->redirs;

    while (*slot != NULL)
        slot = &(*slot)->next;

    *slot = r;
    return 0;
}

/* One redirection: the operator the lexer saw, then the word.
 *
 * The word is a token in its own right, and the lexer put the descriptor in front of it
 * when the operator was `2>` and friends.  A bare `> file` has no such token, so the
 * descriptor is 1 (or 0 for `<`). */
static int parse_redirects(parser_t *p, cmd_t *c) {
    while (at(p, T_REDIR)) {
        redir_kind_t kind = peek(p)->rkind;
        char        *fdtext = NULL;

        p->at++;

        if (at(p, T_WORD)) {
            fdtext = next(p)->text;

            if (fdtext != NULL && fdtext[1] == 0 && fdtext[0] >= '0' && fdtext[0] <= '9') {
                /* The descriptor the lexer put there for `2>`; the word is the next one. */
                char *digit = fdtext;

                fdtext = at(p, T_WORD) ? next(p)->text : NULL;

                if (add_redir(c, kind, fdtext) != 0) {
                    free(digit);
                    return -1;
                }

                free(digit);
                continue;
            }
        }

        if (fdtext == NULL && at(p, T_WORD))
            fdtext = next(p)->text;

        if (fdtext == NULL) {
            fail(p, "a redirection needs a file name");
            return -1;
        }

        if (add_redir(c, kind, fdtext) != 0)
            return -1;
    }

    return 0;
}

/* A simple command: assignments, words, redirections, until something that is not one of
 * those. */
static cmd_t *parse_simple(parser_t *p) {
    cmd_t *c = node(CMD_SIMPLE);

    if (c == NULL)
        return NULL;

    for (;;) {
        if (at(p, T_ASSIGN)) {
            /* Only before the first word.  `echo x=1` sets nothing, and a command that
             * stopped collecting assignments after the first word would make
             * `FOO=1 ls FOO=2` set two things. */
            if (c->nwords == 0) {
                if (add_assign(c, next(p)->text) != 0)
                    goto oom;
            } else {
                if (add_word(c, next(p)->text) != 0)
                    goto oom;
            }

            continue;
        }

        if (at(p, T_WORD) || at(p, T_DPAREN) || at(p, T_BACKTICK)) {
            token_t *t = next(p);

            /* A command substitution lexed as its own token becomes a word whose text is
             * `$(...)`, so that the expander sees one uniform thing to expand.  A backtick
             * becomes `$(...)` too: the two spellings differ in what has to be escaped and
             * in nothing else, and having one code path for "run this and take its output"
             * is worth more than the distinction. */
            if (t->kind == T_DPAREN) {
                char *wrapped = malloc(strlen(t->text) + 4);

                if (wrapped == NULL)
                    goto oom;

                sprintf(wrapped, "$(%s)", t->text);
                free(t->text);
                t->text = wrapped;
                t->kind = T_WORD;
            } else if (t->kind == T_BACKTICK) {
                char *wrapped = malloc(strlen(t->text) + 4);

                if (wrapped == NULL)
                    goto oom;

                sprintf(wrapped, "$(%s)", t->text);
                free(t->text);
                t->text = wrapped;
                t->kind = T_WORD;
            }

            if (add_word(c, t->text) != 0)
                goto oom;

            if (c->nwords > MAX_ARGS) {
                fail(p, "too many arguments");
                goto oom;
            }

            continue;
        }

        if (at(p, T_REDIR)) {
            if (parse_redirects(p, c) != 0)
                goto oom;

            continue;
        }

        break;
    }

    /* `name ()` with nothing after it is a function definition, not a command called
     * `name` with the arguments `()`.  The check is for an *empty* word list, because
     * `name ( x )` is not a function on any shell and treating it as one would swallow a
     * command a person meant to run. */
    if (c->nwords == 1 && c->words[0] != NULL && at(p, T_LPAREN)) {
        size_t save = p->at;
        p->at++;

        if (accept(p, T_RPAREN) && at(p, T_LBRACE)) {
            cmd_t *f = node(CMD_FUNC);

            if (f == NULL)
                goto oom;

            f->name = strdup(c->words[0]);
            p->at++;                       /* past `{` */

            skip_separators(p);
            f->a = parse_and_or(p);

            if (f->a == NULL) {
                cmd_free(f);
                goto oom;
            }

            skip_separators(p);

            if (!accept(p, T_RBRACE)) {
                fail(p, "`}' expected to end a function");
                cmd_free(f);
                goto oom;
            }

            cmd_free(c);
            return f;
        }

        p->at = save;
    }

    if (c->nwords == 0 && c->nassigns == 0 && c->redirs == NULL) {
        /* Nothing at all.  A command that is only redirections is legal and is how
         * `> file` works; this branch is for a bare `}` or a stray keyword. */
        cmd_free(c);
        return NULL;
    }

    return c;

oom:
    cmd_free(c);
    return NULL;
}

/* `( list )` and `{ list ; }`. */
static cmd_t *parse_group(parser_t *p, tok_kind_t closer, cmd_kind_t kind) {
    cmd_t *c = node(kind);

    if (c == NULL)
        return NULL;

    skip_separators(p);
    c->a = parse_and_or(p);

    if (c->a == NULL) {
        cmd_free(c);
        return NULL;
    }

    skip_separators(p);

    if (!accept(p, closer)) {
        failf(p, "a closing bracket was expected",
              closer == T_RPAREN ? ")" : "}");
        cmd_free(c);
        return NULL;
    }

    return c;
}

static cmd_t *parse_if(parser_t *p) {
    cmd_t *c = node(CMD_IF);

    if (c == NULL)
        return NULL;

    p->at++;                               /* `if` */
    skip_separators(p);
    c->a = parse_and_or(p);

    if (c->a == NULL)
        goto oom;

    skip_separators(p);

    if (!accept(p, T_THEN)) {
        fail(p, "`then' expected");
        goto oom;
    }

    skip_separators(p);
    c->b = parse_and_or(p);

    if (c->b == NULL)
        goto oom;

    skip_separators(p);

    while (at(p, T_ELIF)) {
        p->at++;
        skip_separators(p);
        c->b = parse_and_or(p);            /* the condition of the elif */

        if (c->b == NULL)
            goto oom;

        skip_separators(p);

        if (!accept(p, T_THEN)) {
            fail(p, "`then' expected after `elif'");
            goto oom;
        }

        skip_separators(p);
        c->a = c->b;
        c->b = parse_and_or(p);

        if (c->b == NULL)
            goto oom;

        skip_separators(p);
    }

    if (accept(p, T_ELSE)) {
        skip_separators(p);
        c->a = c->b;
        c->b = parse_and_or(p);

        if (c->b == NULL)
            goto oom;

        skip_separators(p);
    }

    if (!accept(p, T_FI)) {
        fail(p, "`fi' expected");
        goto oom;
    }

    return c;

oom:
    cmd_free(c);
    return NULL;
}

static cmd_t *parse_for(parser_t *p) {
    cmd_t *c = node(CMD_FOR);

    if (c == NULL)
        return NULL;

    p->at++;                               /* `for` */

    if (!at(p, T_WORD)) {
        fail(p, "a loop variable name was expected after `for'");
        goto oom;
    }

    c->name = strdup(next(p)->text);

    skip_separators(p);

    if (accept(p, T_IN)) {
        /* The word list, which may run to the next line's `do`.  This is why expansion
         * cannot happen here: `for f in *` has to glob when the loop *runs*, not when it
         * is read, or a file created inside the loop would never be seen. */
        while (at(p, T_WORD) || at(p, T_DPAREN) || at(p, T_BACKTICK) ||
               at(p, T_ASSIGN)) {
            token_t *t = next(p);

            if (t->kind == T_DPAREN || t->kind == T_BACKTICK) {
                char *wrapped = malloc(strlen(t->text) + 4);

                if (wrapped == NULL)
                    goto oom;

                sprintf(wrapped, "$(%s)", t->text);
                free(t->text);
                t->text = wrapped;
                t->kind = T_WORD;
            }

            if (add_word(c, t->text) != 0)
                goto oom;

            skip_separators(p);
        }
    } else {
        c->no_in = 1;
    }

    if (!accept(p, T_DO)) {
        fail(p, "`do' expected");
        goto oom;
    }

    skip_separators(p);
    c->a = parse_and_or(p);

    if (c->a == NULL)
        goto oom;

    skip_separators(p);

    if (!accept(p, T_DONE)) {
        fail(p, "`done' expected");
        goto oom;
    }

    return c;

oom:
    cmd_free(c);
    return NULL;
}

static cmd_t *parse_while(parser_t *p, int negated) {
    cmd_t *c = node(CMD_WHILE);

    if (c == NULL)
        return NULL;

    c->negated = negated;
    p->at++;                               /* `while` or `until` */
    skip_separators(p);
    c->a = parse_and_or(p);

    if (c->a == NULL)
        goto oom;

    skip_separators(p);

    if (!accept(p, T_DO)) {
        fail(p, "`do' expected");
        goto oom;
    }

    skip_separators(p);
    c->b = parse_and_or(p);

    if (c->b == NULL)
        goto oom;

    skip_separators(p);

    if (!accept(p, T_DONE)) {
        fail(p, "`done' expected");
        goto oom;
    }

    return c;

oom:
    cmd_free(c);
    return NULL;
}

/* One `pattern) list` arm.
 *
 * The patterns are kept as typed, quotes and all, and matched with `fnmatch` at run time
 * -- so `case $x in [ab]*)` has the same meaning as `case $x in [ab]*)` on every other
 * shell, and `case $x in "$y"*)` does not, because the quotes are still there. */
static int parse_case_arm(parser_t *p, case_arm_t *arm) {
    int n = 0;

    for (;;) {
        if (at(p, T_WORD) || at(p, T_DPAREN) || at(p, T_BACKTICK)) {
            token_t *t = next(p);
            char    *text = t->text;
            char   **grown;

            if (t->kind == T_DPAREN || t->kind == T_BACKTICK) {
                text = malloc(strlen(t->text) + 4);

                if (text == NULL)
                    return -1;

                sprintf(text, "$(%s)", t->text);
                free(t->text);
            }

            grown = realloc(arm->patterns, (size_t)(n + 2) * sizeof(char *));

            if (grown == NULL) {
                free(text);
                return -1;
            }

            arm->patterns = grown;
            arm->patterns[n++] = text;
            arm->patterns[n] = NULL;
            continue;
        }

        if (accept(p, T_PIPE))
            continue;

        break;
    }

    if (n == 0)
        return -1;

    if (!accept(p, T_RPAREN))
        return -1;

    skip_separators(p);
    arm->body = parse_and_or(p);
    return arm->body != NULL ? 0 : -1;
}

static cmd_t *parse_case(parser_t *p) {
    cmd_t *c = node(CMD_CASE);

    if (c == NULL)
        return NULL;

    p->at++;                               /* `case` */

    if (!at(p, T_WORD)) {
        fail(p, "a word was expected after `case'");
        goto oom;
    }

    c->name = strdup(next(p)->text);

    skip_separators(p);

    if (!accept(p, T_IN)) {
        fail(p, "`in' expected");
        goto oom;
    }

    skip_separators(p);

    /* An empty case body is legal and is a no-op, which is what `case $x in esac` means on
     * every shell; refusing it would make `case` unusable for "is there a default?" */
    while (!at(p, T_ESAC) && !at(p, T_EOF)) {
        case_arm_t *grown = realloc(c->arms, (size_t)(c->narms + 1) * sizeof(case_arm_t));

        if (grown == NULL)
            goto oom;

        c->arms = grown;
        memset(&c->arms[c->narms], 0, sizeof(case_arm_t));

        if (parse_case_arm(p, &c->arms[c->narms]) != 0) {
            fail(p, "a case arm must be patterns followed by `)'");
            goto oom;
        }

        c->narms++;

        /* `;;` ends the arm.  `;&` ends it and falls into the next one, which is a
         * different operator and is not implemented; a lone `;` is accepted because every
         * shell accepts it and a script written for one should run here. */
        if (accept(p, T_DSEMI))
            c->arms[c->narms - 1].taken = 1;
        else if (at(p, T_SEMI))
            p->at++;

        skip_separators(p);

        if (c->narms > MAX_ARMS) {
            fail(p, "too many case arms");
            goto oom;
        }
    }

    if (!accept(p, T_ESAC)) {
        fail(p, "`esac' expected");
        goto oom;
    }

    return c;

oom:
    cmd_free(c);
    return NULL;
}

/* A compound command, or nothing. */
static cmd_t *parse_compound(parser_t *p) {
    if (accept(p, T_IF))
        return parse_if(p);

    if (accept(p, T_FOR))
        return parse_for(p);

    if (accept(p, T_WHILE))
        return parse_while(p, 0);

    if (accept(p, T_UNTIL))
        return parse_while(p, 1);

    if (accept(p, T_CASE))
        return parse_case(p);

    if (accept(p, T_LBRACE)) {
        cmd_t *c = parse_group(p, T_RBRACE, CMD_GROUP);

        /* A `{` must be followed by a `;` or a newline before its body, and insisting on
         * it is what stops `{ echo hi }` from working.  It is also what distinguishes a
         * brace group from a word beginning with `{`, which is why the lexer only makes
         * `T_LBRACE` out of a `{` that stands alone. */
        return c;
    }

    if (at(p, T_LPAREN)) {
        p->at++;
        return parse_group(p, T_RPAREN, CMD_SUB);
    }

    return NULL;
}

static cmd_t *parse_pipeline(parser_t *p) {
    cmd_t *first = NULL;
    cmd_t *pipe  = NULL;

    for (;;) {
        cmd_t *stage;

        if (at(p, T_LBRACE)) {
            p->at++;
            stage = parse_group(p, T_RBRACE, CMD_GROUP);
        } else {
            stage = parse_compound(p);
        }

        if (stage == NULL) {
            skip_separators(p);
            stage = parse_simple(p);
        }

        if (stage == NULL)
            return NULL;

        /* Redirections after a compound: `if ...; fi > file`.  A simple command takes its
         * own inside `parse_simple`, so only the compound case reaches here. */
        if (at(p, T_REDIR)) {
            if (parse_redirects(p, stage) != 0) {
                cmd_free(stage);
                return NULL;
            }
        }

        if (first == NULL) {
            first = stage;
        } else {
            cmd_t **grown = realloc(pipe->stages,
                                    (size_t)(pipe->nstages + 1) * sizeof(cmd_t *));

            if (grown == NULL) {
                cmd_free(stage);
                cmd_free(pipe);
                return NULL;
            }

            pipe->stages = grown;
            pipe->stages[pipe->nstages++] = stage;
        }

        skip_separators(p);

        if (!at(p, T_PIPE))
            break;

        p->at++;
        skip_separators(p);

        if (pipe == NULL) {
            pipe = node(CMD_PIPE);

            if (pipe == NULL) {
                cmd_free(first);
                return NULL;
            }
        }

        if (pipe->nstages >= MAX_STAGES - 1) {
            fail(p, "too many stages in a pipeline");
            cmd_free(first);
            cmd_free(pipe);
            return NULL;
        }
    }

    if (pipe != NULL) {
        /* The first stage goes in as `a` rather than as stages[0], so that a two-stage
         * pipeline -- by far the common one -- is a node with one child rather than a node
         * with an array of two.  It is also what makes `a | b` and `a | b | c` the same
         * shape, which matters for nothing except the reader. */
        cmd_t **grown = realloc(pipe->stages,
                                (size_t)(pipe->nstages + 2) * sizeof(cmd_t *));

        if (grown == NULL) {
            cmd_free(first);
            cmd_free(pipe);
            return NULL;
        }

        pipe->stages = grown;
        memmove(&pipe->stages[1], &pipe->stages[0],
                (size_t)pipe->nstages * sizeof(cmd_t *));
        pipe->stages[0] = first;
        pipe->nstages++;
        return pipe;
    }

    return first;
}

static cmd_t *parse_and_or(parser_t *p) {
    cmd_t *left = parse_pipeline(p);

    if (left == NULL)
        return NULL;

    for (;;) {
        tok_kind_t op = peek(p)->kind;

        if (op != T_AND_IF && op != T_OR_IF && op != T_SEMI && op != T_NEWLINE)
            break;

        int background = op == T_SEMI && peek(p)->background;

        p->at++;
        skip_separators(p);

        /* A trailing `&`, `&&` or `;` at the end of the input is a command with nothing
         * after it, and that is an error rather than a silent no-op -- except for a bare
         * `&`, which every shell allows and which means "run this and do not wait". */
        if (at(p, T_EOF)) {
            if (op == T_AND_IF || op == T_OR_IF) {
                fail(p, "a command was expected after this operator");
                cmd_free(left);
                return NULL;
            }

            break;
        }

        cmd_t *right = parse_pipeline(p);

        if (right == NULL) {
            cmd_free(left);
            return NULL;
        }

        cmd_t *c = node(CMD_LIST);

        if (c == NULL) {
            cmd_free(left);
            cmd_free(right);
            return NULL;
        }

        c->a = left;
        c->b = right;

        if (op == T_AND_IF)
            c->kind = CMD_LIST, c->negated = 0, c->name = strdup("&&");
        else if (op == T_OR_IF)
            c->kind = CMD_LIST, c->negated = 1, c->name = strdup("||");
        else
            c->kind = CMD_LIST, c->negated = 0, c->name = strdup(background ? "&" : ";");

        left = c;
        skip_separators(p);
    }

    return left;
}

cmd_t *parse_program(tokens_t *toks, const char **err) {
    parser_t p = { toks, 0, NULL };
    cmd_t   *c;

    *err = NULL;

    skip_separators(&p);

    if (at(&p, T_EOF))
        return NULL;                      /* an empty line is not an error */

    c = parse_and_or(&p);

    if (c == NULL) {
        if (p.err == NULL)
            p.err = "the line could not be parsed";

        *err = p.err;
        return NULL;
    }

    skip_separators(&p);

    if (!at(&p, T_EOF)) {
        cmd_free(c);
        *err = "something after the end of the command";
        return NULL;
    }

    return c;
}
