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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    tokens_t     *t;
    int           at;
    const char   *err;

    /* Which keyword ends the list currently being parsed, or 0 for "the end of the input".
     *
     * A single list of closers does not work, and the reason is that the closers belong to
     * *different* constructs.  A `for` body ends at `done` and must sail straight past a
     * `fi` belonging to an `if` nested inside it -- `for i in 1 2; do if C; then A; fi; echo
     * B; done` is one loop with one command in its body.  An `if` body ends at `fi`, `elif`
     * or `else`, and must sail past a `done` belonging to a loop inside it.
     *
     * So each construct says which words close *its* list, while it is parsing it, and
     * restores the previous answer afterwards.  It is a field on the parser rather than an
     * argument because `parse_and_or()` recurses through five call paths, and threading it
     * through every one of them would make the parameter list worse than the state.
     *
     * It is a mask rather than one word because `if` needs three and `case` needs two, and
     * because `}` and `)` close the group forms. */
    unsigned      closers;
} parser_t;

/* The words that end each construct's list. */
#define CLOSE_FI      (1u << 0)   /* `fi`, `elif`, `else`  -- an `if` body          */
#define CLOSE_DONE    (1u << 1)   /* `done`                 -- a `for`/`while` body */
#define CLOSE_ESAC    (1u << 2)   /* `esac`                 -- a `case` arm list    */
#define CLOSE_BRACE   (1u << 3)   /* `}`                    -- a `{ }` group        */
#define CLOSE_PAREN   (1u << 4)   /* `)`                    -- a `( )` group        */

/* The end of the input, as a closer.
 *
 * It is not one of the words above because it is not a word: it is where a list ends when
 * nothing else closes it, which is the top level and every construct's *condition*.  Carrying
 * it as a bit rather than testing `at(p, T_EOF)` at each call site keeps the question "what
 * ends this list?" in one place, and it is what makes `echo a; echo b; echo c` work: every
 * `;` joins, and only the final end of input ends the list. */
#define CLOSE_INPUT   (1u << 5)

/* A few words, bounded.  A command line longer than this is refused with a message rather
 * than silently truncated, which is the one thing a parser must not do. */
#define MAX_ARGS   512
#define MAX_STAGES 64
#define MAX_ARMS   128

static cmd_t *parse_and_or(parser_t *p);

/* Does this token end the list being parsed?
 *
 * `fi`, `done`, `esac`, `}` and `)` all end a construct, and a separator in front of one of
 * them is the end of a list rather than a join to another command.  Asking the question
 * here rather than at each call site is what keeps the list of closers in one place: when
 * `elif` was added, the four older closers did not each have to be remembered.
 *
 * Which closers count is `p->closers`, not a fixed list -- see the field's comment for why
 * `for` and `if` cannot share one.  `then` and `do` are the other direction: they *begin*
 * a construct's tail and follow a separator exactly as `;` between two commands does, so
 * they are in the same test.  Without `do` in it, `while [ x ]; do` parsed its condition,
 * hit the `;`, and demanded a command after it -- and found `do`, which is not one.
 *
 * `fi`, `done`, `esac`, `}` and `)` all end a construct, and a separator in front of one
 * of them is the end of a list rather than a join to another command.  Asking the question
 * here rather than at each call site is what keeps the list of closers in one place: when
 * `elif` was added, the four older closers did not each have to be remembered.
 *
 * `then`, `else`, `elif` and `do` are here too, for the same reason from the other side:
 * they *begin* the tail of a construct, and they follow a separator exactly as a `;`
 * between two commands does.  Nothing in the token stream distinguishes the two, so the
 * word after the separator is what tells them apart -- and a loop body that ends in a
 * nested `if` (`do if C; then A; fi; echo B; done`) needs the inner `; fi` to close the `if`
 * without the list inside it also deciding it has reached the end of the loop.
 *
 * `elif` *is* here, which looks wrong until the `;` before it is considered: `if C; then
 * A; elif D; then B; fi` puts `elif` after a separator just as `do` is, and it continues
 * the construct rather than ending it -- which is why this list is about "a separator
 * followed by one of these ends the *command list*", not about nesting. */
static int at_closer(parser_t *p) {
    tok_kind_t k = p->t->v[p->at].kind;

    /* `then` and `do` are here, and they are not closers of anything.  They *begin* the
     * tail of a construct and follow a separator exactly as the `;` between two commands
     * does, so the list stops in front of them and the construct's own parser takes them.
     * That is why this is a mask rather than a list of names: the caller says which of its
     * own closers it is inside, and these two always count because they can only follow a
     * separator if a construct is waiting for one. */
    if (k == T_THEN || k == T_DO)
        return 1;

    if (k == T_FI || k == T_ELIF || k == T_ELSE)
        return (p->closers & CLOSE_FI) != 0;

    if (k == T_DONE)
        return (p->closers & CLOSE_DONE) != 0;

    if (k == T_ESAC)
        return (p->closers & CLOSE_ESAC) != 0;

    if (k == T_RBRACE)
        return (p->closers & CLOSE_BRACE) != 0;

    if (k == T_RPAREN)
        return (p->closers & CLOSE_PAREN) != 0;

    return k == T_EOF && (p->closers & CLOSE_INPUT) != 0;
}

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

static const char *kind_name(cmd_kind_t k) {
    switch (k) {
    case CMD_SIMPLE: return "simple";
    case CMD_LIST:   return "list";
    case CMD_PIPE:   return "pipe";
    case CMD_SUB:    return "sub";
    case CMD_GROUP:  return "group";
    case CMD_IF:     return "if";
    case CMD_FOR:    return "for";
    case CMD_WHILE:  return "while";
    case CMD_CASE:   return "case";
    case CMD_FUNC:   return "func";
    }

    return "?";
}

/* A parsed command as an indented tree.
 *
 * Accept-or-reject is one bit per line and it cannot tell a right tree from a wrong one, so
 * anything that tests this parser needs to be able to *look* at what it built.  That is how
 * the `elif` defect was pinned down: `if true then A elif true then B fi` parsed cleanly and
 * still ran B, because the chain had overwritten itself as it was read and the tree held
 * only the last branch.  No accept/reject test can see that.
 *
 * It is in the shell rather than in the test rig because the guest needs it too: a script
 * that does not work should be able to show the tree the parser built, next to the
 * complaint about it. */
void cmd_tree_dump(cmd_t *c, int depth) {
    if (c == NULL) {
        printf("%*s(null)\n", depth * 2, "");
        return;
    }

    printf("%*s%s", depth * 2, "", kind_name(c->kind));

    if (c->name != NULL)
        printf(" name=%s", c->name);

    if (c->negated)
        printf(" negated");

    if (c->no_in)
        printf(" no_in");

    printf("\n");

    for (int i = 0; i < c->nwords; i++)
        printf("%*sword: %s\n", (depth + 1) * 2, "", c->words[i]);

    for (int i = 0; i < c->nassigns; i++)
        printf("%*sassign: %s\n", (depth + 1) * 2, "", c->assigns[i]);

    for (redir_t *r = c->redirs; r != NULL; r = r->next)
        printf("%*sredir: %s\n", (depth + 1) * 2, "", r->word != NULL ? r->word : "");

    for (int i = 0; i < c->nstages; i++) {
        printf("%*sstage %d:\n", (depth + 1) * 2, "", i);
        cmd_tree_dump(c->stages[i], depth + 2);
    }

    for (int i = 0; i < c->narms; i++) {
        printf("%*sarm %d:\n", (depth + 1) * 2, "", i);

        for (int k = 0; c->arms[i].patterns != NULL && c->arms[i].patterns[k] != NULL; k++)
            printf("%*spattern: %s\n", (depth + 2) * 2, "", c->arms[i].patterns[k]);

        cmd_tree_dump(c->arms[i].body, depth + 2);
    }

    for (cmd_t *e = c->elifs; e != NULL; e = e->next_elif) {
        printf("%*selif condition:\n", (depth + 1) * 2, "");
        cmd_tree_dump(e->a, depth + 2);
        printf("%*selif body:\n", (depth + 1) * 2, "");
        cmd_tree_dump(e->b, depth + 2);
    }

    if (c->a != NULL) {
        printf("%*sa:\n", (depth + 1) * 2, "");
        cmd_tree_dump(c->a, depth + 2);
    }

    if (c->b != NULL) {
        printf("%*sb:\n", (depth + 1) * 2, "");
        cmd_tree_dump(c->b, depth + 2);
    }
}

void cmd_free(cmd_t *c) {
    if (c == NULL)
        return;

    /* Recursive, and that is fine: the tree's depth is the nesting depth of the input,
     * which for anything a person types is under twenty.
     *
     * Each pointer is freed exactly once.  `c->stages` and `c->arms` were each released
     * twice -- once after the loop that frees what they point *at*, and again in the list
     * at the bottom -- and a block that has already gone back on the allocator's free list
     * is still a perfectly good-looking address, so the second release was accepted,
     * merged the wrong neighbour into it, and put a chain through the free list that every
     * later allocation walked.
     *
     * What that looked like from the outside: a shell whose `$( ... )` children ran for a
     * while and then found a free-list node holding an address in the shell's own text, so
     * `free()` walked into `.text` and faulted.  Every report pointed at the allocator or
     * at the kernel, and the two actual symptoms -- the faulting address changing from run
     * to run, and the kernel's own `memcpy` guard never firing -- were both the free list
     * being a shape nobody could have written on purpose.
     *
     * The allocator now poisons a freed block and checks it when the block is handed out
     * again, and carries a redzone past the end of every block, so the second release is
     * reported where it happens instead of surfacing as corruption later. */
    for (int i = 0; i < c->nwords; i++)
        free(c->words[i]);

    for (int i = 0; i < c->nassigns; i++)
        free(c->assigns[i]);

    for (redir_t *r = c->redirs; r != NULL;) {
        redir_t *next = r->next;

        /* `r->word` is a `strdup()` of the token's text -- `add_redir()` copies it, because
         * the file name has to outlive the tokens -- so it is owned here and freed here. */
        free(r->word);
        free(r);
        r = next;
    }

    for (int i = 0; i < c->nstages; i++)
        cmd_free(c->stages[i]);

    /* The arm's pattern strings are *borrowed*, like a redirection's file name: they are the
     * lexer's tokens' text, and `tokens_free()` releases them after the parse.  They are
     * matched with `fnmatch()` at run time against the pattern as typed, quotes and all,
     * which is why they are not copied -- but it does mean this loop must not free them. */
    for (int i = 0; i < c->narms; i++) {
        free(c->arms[i].patterns);
        cmd_free(c->arms[i].body);
    }

    /* Each `elif` pair is a tree of its own -- a condition and a body, both of which can
     * be compound commands -- so they are freed like any other subtree rather than by
     * walking two extra links here. */
    for (cmd_t *e = c->elifs; e != NULL; ) {
        cmd_t *next = e->next_elif;

        cmd_free(e);
        e = next;
    }

    /* The `else` branch is an ordinary subtree, so it is freed like one rather than by
     * walking it here -- which is also why it needed a field of its own: a field is what
     * makes it findable, and a tree that cannot be found cannot be freed. */
    cmd_free(c->orelse);

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
 * The word is a token in its own right, and the lexer puts the descriptor in front of it
 * when the operator was `2>` and friends.  A bare `> file` has no such token, so the
 * descriptor is 1 (or 0 for `<`).
 *
 * Whether the token in front is *that* descriptor or the redirection's own argument is
 * something the lexer knows and this cannot work out: both are a `T_WORD` whose text is one
 * digit.  So the lexer says which, in `token_t.fd_prefixed`, and this asks.
 *
 * Guessing from the text -- "if it is a single digit, it must be the prefixed descriptor" --
 * is what made `echo hi >&2` print nothing at all.  The `2` was taken as a prefix, the
 * next token was the end of the line, and the redirection was built with *no* argument;
 * `collect_redirs()` then read `strtol("")` as zero and made it `>&0`.  So the command's
 * standard output became its standard input, and the output went nowhere.
 *
 * `>&2` is how a script sends something to the console, and a script that sent something
 * there lost it. */
static int parse_redirects(parser_t *p, cmd_t *c) {
    while (at(p, T_REDIR)) {
        redir_kind_t kind = peek(p)->rkind;
        char        *fdtext = NULL;

        p->at++;

        if (at(p, T_WORD)) {
            token_t *t = next(p);

            if (t->fd_prefixed) {
                /* The descriptor the lexer put there for `2>`; the word is the next one.
                 *
                 * The prefixed token is *borrowed*, never freed.  It is a token's text and
                 * the token still holds it: `tokens_free()` walks the token list after the
                 * parse and frees every one of those, so freeing it here released the same
                 * block twice.  The two frees were minutes apart in a guest run -- this one
                 * during the parse, that one after the line had run -- which is why it looked
                 * like the heap was being corrupted by something else entirely.
                 *
                 * Nothing here needs it afterwards, which is why the omission is safe and not
                 * a leak: the token owns it and the token is freed. */
                fdtext = at(p, T_WORD) ? next(p)->text : NULL;

                if (add_redir(c, kind, fdtext) != 0)
                    return -1;

                continue;
            }

            fdtext = t->text;
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
                /* One wrap serves both spellings, and that is not a coincidence.
                 *
                 * The lexer's token holds what was between the first `(` and the `)` that
                 * matches it: `ls` for `$(ls)`, and `(n + 1)` for `$((n + 1))` -- in the
                 * arithmetic form the outer pair *is* the operator and the inner pair is
                 * part of the expression, so the text already carries one set of
                 * parentheses.  Putting `$(` and `)` around it therefore gives `$(ls)` and
                 * `$((n + 1))` respectively, from the same two characters and no test. */
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
                cmd_free(c);
                return NULL;
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

    /* Inside the group, so that a `;` followed by `}` or `)` ends it rather than joining to
     * another command.  A `;` followed by a `done` or a `fi` in the body still closes a
     * nested construct rather than this one, which is the reason this is a field on the
     * parser and not a test on the token: see `parser_t.closers`. */
    unsigned      was = p->closers;

    p->closers = closer == T_RPAREN ? CLOSE_PAREN : CLOSE_BRACE;

    /* An empty body is a group with nothing in it, not a syntax error: `while ...; do done`
     * is a loop that never runs, and `true && { }` is a group that does nothing.  Both are
     * legal, both are what a person means, and rejecting them makes the parser stricter
     * than every shell it is compared with.
     *
     * What is *not* legal is a body that failed to parse, and `parse_and_or()` answers
     * NULL for both cases.  `p->err` is what tells them apart: an empty body leaves it
     * NULL, and anything it could not parse set it on the way out. */
    const char *why = p->err;

    c->a = parse_and_or(p);

    if (c->a == NULL) {
        if (p->err != why || !at(p, closer)) {
            cmd_free(c);
            return NULL;
        }
    } else {
        skip_separators(p);
    }

    p->closers = was;

    if (!accept(p, closer)) {
        failf(p, "a closing bracket was expected",
              closer == T_RPAREN ? ")" : "}");
        cmd_free(c);
        return NULL;
    }

    return c;
}

/* The `if` has already been consumed.
 *
 * `parse_compound()` reaches here through `accept(p, T_IF)`, which advances past the
 * keyword.  Advancing again skips the token *after* it -- for `if true; then ...` that is
 * `true` -- so the condition is parsed from `;`, `then` swallows what should have been the
 * condition, and the line is reported as "something after the end of the command".
 *
 * Every compound construct failed that way, and the report said the same sentence for all
 * of them, so nothing said which construct or which token.  A line is now quoted with the
 * error, which is what makes this readable from the outside: the error is about a line, and
 * the line belongs in the report. */
static cmd_t *parse_if(parser_t *p) {
    cmd_t *c = node(CMD_IF);

    if (c == NULL)
        return NULL;

    skip_separators(p);
    c->a = parse_and_or(p);

    if (c->a == NULL)
        goto oom;

    skip_separators(p);

    if (!accept(p, T_THEN)) {
        fail(p, "`then' expected");
        goto oom;
    }

    /* Everything from here to `fi` is inside the `if`, so a `;` followed by a `done` or an
     * `esac` in there closes a nested construct and not this one. */
    unsigned      was = p->closers;

    p->closers = CLOSE_FI;

    /* `why_then` is how an empty then-branch is told from a branch that failed to parse:
     * `if C; then; fi` is legal and does nothing, and `parse_and_or()` answers NULL for
     * both that and a real error.  An empty branch leaves `p->err` alone.
     *
     * The then-branch goes in `c->b`, and the *else*-branch replaces it below -- `b` is
     * "the branch that runs", and there is only one of those.  So a node with no else ends
     * up with `b` holding the then-branch, and the `elif` chain hangs off `elifs`. */
    const char *why_then = p->err;

    c->b = parse_and_or(p);

    if (c->b == NULL && (p->err != why_then || !at_closer(p)))
        goto oom;

    skip_separators(p);

    /* Each `elif` is a condition, a `then`, and a body, and the branch it introduces runs when
     * it is the *first* condition that holds -- which is what POSIX says and what a person
     * means, and neither is what the tree used to do.
     *
     * A `CMD_IF` holds one condition and one branch, so an `elif` cannot be a sibling of the
     * `if`: there is nowhere to put it.  Each pair is a `CMD_LIST` named `elif` whose `a` is
     * the condition and whose `b` the body, and `c->elifs` chains them in source order.
     *
     * The chain used to overwrite `c->a` and `c->b` as it read, so only the *last* branch
     * survived: `if true; then A; elif true; then B; fi` parsed cleanly and ran B.  A parse
     * test cannot see that -- the tree is well formed either way -- which is why the host
     * test compares the words in the tree against the words on the line and not only
     * accept/reject.
     *
     * The executor takes the first condition that holds and stops there, so a condition after
     * the branch that was taken is never evaluated, which is what POSIX requires and what
     * makes `elif [ -f x ]` after a matching earlier branch safe. */
    while (at(p, T_ELIF)) {
        cmd_t *cond;

        p->at++;
        skip_separators(p);
        cond = parse_and_or(p);            /* the condition of the elif */

        if (cond == NULL)
            goto oom;

        skip_separators(p);

        if (!accept(p, T_THEN)) {
            fail(p, "`then' expected after `elif'");
            goto oom;
        }

        skip_separators(p);
        cmd_t *body = parse_and_or(p);

        if (body == NULL) {
            if (!at_closer(p)) {
                cmd_free(cond);
                goto oom;
            }
        } else {
            skip_separators(p);
        }

        /* Fold the pair into the list hanging off `c->a`'s slot for `else` branches: a
         * `CMD_IF` whose `b` is a `CMD_LIST` of `{ condition, body }` pairs.  The
         * executor walks it in order and stops at the first condition that holds. */
        cmd_t *pair = node(CMD_LIST);

        if (pair == NULL) {
            cmd_free(cond);
            cmd_free(body);
            goto oom;
        }

        pair->kind = CMD_LIST;
        pair->a = cond;
        pair->b = body;
        pair->name = strdup("elif");

        if (c->elifs == NULL) {
            c->elifs = pair;
        } else {
            cmd_t *tail = c->elifs;

            while (tail->next_elif != NULL)
                tail = tail->next_elif;

            tail->next_elif = pair;
        }
    }

    /* The else-branch gets a field of its own, because `a` and `b` are both taken.
     *
     * `a` is the condition and `b` is the branch that runs when it holds, so there is no
     * slot left for "the branch that runs when it does not" -- and putting the else-branch
     * in `b` made `if C; then A; else B; fi` run `B` when `C` held, which is the opposite of
     * what the line says.
     *
     * A separate field is also what keeps the three apart: condition, then-branch, and the
     * `elif` chain, with `orelse` for the alternative.  Before this the tree had two slots
     * for three things and every arrangement of them was wrong in some case.
     *
     * An empty else-branch is legal -- `if C; then A; else; fi` -- and leaves the field
     * NULL, which the executor treats as "do nothing", the same answer an empty branch
     * would give. */
    if (accept(p, T_ELSE)) {
        const char *why_else = p->err;

        skip_separators(p);
        c->orelse = parse_and_or(p);

        if (c->orelse == NULL) {
            if (p->err != why_else || !at_closer(p)) {
                cmd_free(c);
                return NULL;
            }
        } else {
            skip_separators(p);
        }
    }

    p->closers = was;

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

    /* `for` was consumed by parse_compound()'s accept(); see parse_if().
     *
     * The variable name is taken with `next()` rather than by insisting on a `T_WORD`,
     * because a variable may be called anything a word may be called -- including the name
     * of a shell keyword.  `for do in a b; do echo $do; done` is legal on bash and on every
     * other shell, and it worked here only by accident: `do` followed `for`, so it was
     * lexed as the keyword `T_DO`, and this parser refused the line with "a loop variable
     * name was expected after `for'".  The keyword's *spelling* is what matters in this
     * position, not its kind, so the text is taken and the kind is not. */
    if (peek(p)->kind == T_EOF) {
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

            /* Newlines only, and this is the reason the word list is a loop and not a
             * "scan until the next `do`".
             *
             * `for i in\n  1 2 3\ndo` is legal and the words are on their own lines, so a
             * newline cannot end the list.  A `;` can: `for i in; do` has an empty word
             * list, which is not the same as `for i in` with no `in` at all, and skipping
             * the `;` here would leave `accept(T_DO)` looking at the `;` itself. */
            while (at(p, T_NEWLINE))
                p->at++;
        }
    } else {
        c->no_in = 1;
    }

    /* The `;` between the word list and the `do` -- or the newline, for a list written on
     * its own lines.  This is the one place separators are skipped after a word list,
     * because the list is ended by a *separator* and every other list is ended by a word
     * (`fi`, `done`, `esac`) that its own parser knows to expect. */
    skip_separators(p);

    if (!accept(p, T_DO)) {
        fail(p, "`do' expected");
        goto oom;
    }

    skip_separators(p);

    /* Everything from here to `done` is inside the loop, so a `;` followed by a `fi` in
     * there closes an inner `if` rather than this loop.  See `parser_t.closers`. */
    unsigned      was = p->closers;

    p->closers = CLOSE_DONE;

    /* An empty body is legal: `for i in 1 2; do; done` is a loop that does nothing, which is
     * what a person writes to mean "nothing to do". */
    const char *why_body = p->err;

    c->a = parse_and_or(p);

    if (c->a == NULL && (p->err != why_body || !at_closer(p)))
        goto oom;

    p->closers = was;

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
    /* `while` or `until` was consumed by parse_compound()'s accept(); see parse_if(). */
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

    /* Everything from here to `done` is inside the loop, so a `;` followed by a `fi` in
     * there closes an inner `if` rather than this loop.  See `parser_t.closers`. */
    unsigned      was = p->closers;

    p->closers = CLOSE_DONE;

    /* An empty body is legal: `while read l; do; done` is a loop that reads and does
     * nothing, and rejecting it makes `while` unusable for draining a file. */
    const char *why_body = p->err;

    c->b = parse_and_or(p);

    if (c->b == NULL && (p->err != why_body || !at_closer(p)))
        goto oom;

    p->closers = was;

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

    /* The body is a full command list, and a `;` inside it joins as it does anywhere else.
     *
     * This was briefly made to *end* the arm at a bare `;`, on the reasoning that
     * `case a in a) echo 1; b) echo 2; esac` needs it.  That line is a syntax error on bash,
     * dash and busybox -- the arm has to be closed with `;;` or `;&` -- so the rule would
     * have made this shell accept something it should refuse while still refusing what it
     * should accept: `case $x in a) true && echo hi; esac` is legal on every one of them,
     * and the `&&` there is a list, not an arm terminator.
     *
     * The rule as it stands: inside a `case` arm, `;` behaves exactly as it does
     * everywhere else, and the arm ends at `;;`, at `;&`, or at a newline.
     *
     * An *empty* body is legal and is a no-op, which is what `case $x in a) ;; esac` means:
     * a pattern that matches and does nothing.  It is how a `case` says "these are the
     * interesting ones, ignore the rest", and refusing it makes a deliberate no-op arm
     * unusable.
     *
     * `parse_and_or()` answers NULL for that and for a body that failed to parse, and
     * `p->err` is what tells them apart: an empty body leaves it alone. */
    const char *why = p->err;

    arm->body = parse_and_or(p);

    return (arm->body != NULL || p->err == why) ? 0 : -1;
}

static cmd_t *parse_case(parser_t *p) {
    cmd_t *c = node(CMD_CASE);

    if (c == NULL)
        return NULL;

    /* `case` was consumed by parse_compound()'s accept(); see parse_if(). */
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

    /* Everything from here to `esac` is inside the `case`. */
    unsigned      was = p->closers;

    p->closers = CLOSE_ESAC;

    /* An empty case body is legal and is a no-op, which is what `case $x in esac` means on
     * every shell; refusing it would make `case` unusable for "is there a default?" */
    while (!at(p, T_ESAC) && !at(p, T_EOF)) {
        /* The array is grown by *two* arms' worth, and the extra one is why.
         *
         * The arm being filled in has to stay at `c->arms[c->narms]`, because
         * `parse_case_arm()` writes through a pointer into it -- and if the `realloc()`
         * below moved the array, that pointer would be into freed memory.  Growing by one
         * and filling the new last slot works only because the fill happens after the
         * realloc, with the pointer taken from the *new* array.  What it does not survive
         * is a second arm: the first arm's `taken` is written at `c->arms[narms - 1]`
         * after `c->narms++`, which is the slot the *next* arm is about to be zeroed into
         * by `memset` -- so `taken` was being set on an arm that had already been reset,
         * and a third arm wrote past the end of the array entirely.
         *
         * The visible symptom was the shell's allocator reporting a write one byte past a
         * block, at a caller in `cmd_free()`: the damage had been done by the parse and
         * only discovered when the tree was torn down. */
        if (c->narms + 1 >= MAX_ARMS) {
            fail(p, "too many case arms");
            goto oom;
        }

        size_t want = (size_t)(c->narms + 2) * sizeof(case_arm_t);
        case_arm_t *grown = realloc(c->arms, want);

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
        /* `;;` ends the arm, and `;&` ends it and falls into the next one -- the second is
         * accepted and behaves like the first, which is the honest thing to do with an
         * operator this shell does not implement rather than a syntax error for a script
         * that used it. */
        /* `;;` ends the arm.  `;&` ends it and falls into the next one, which is a
         * different operator and is not implemented; a lone `;` is accepted because every
         * shell accepts it and a script written for one should run here. */
        if (accept(p, T_DSEMI))
            c->arms[c->narms - 1].taken = 1;
        else if (at(p, T_SEMI))
            p->at++;

        /* Newlines only, and deliberately not `skip_separators()`.
         *
         * The `;` that ended the arm's body was consumed by `parse_and_or()`, which is
         * what joins commands with it, so a `case` written one arm per line still has its
         * newlines to get past -- and skipping a `;` that is not there would eat the first
         * pattern of the arm after it.  `case a in a) echo 1; b) echo 2; esac` is a syntax
         * error on bash, dash and busybox alike; what it needs to be is a *readable* error
         * about the `case`, and the message it used to get named a pattern instead. */
        while (at(p, T_NEWLINE))
            p->at++;
    }

    p->closers = was;

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

    /* The closers a pipeline's *stage* can be ended by, which is all five of them.
     *
     * A stage is one command, and it may be any construct: `do if C; then A; fi | p; done`
     * is a loop whose body is a pipeline whose first stage is an `if`.  The stage therefore
     * has to accept a `fi` after a separator whether or not it is inside an `if` of its
     * own -- the `if` parser is what checks that a `fi` belongs to *it*, not the list
     * parser's job to withhold it.
     *
     * The saved mask is what keeps the loop's own `done` out of it: a stage ending at
     * `fi` does not mean the loop's body may end at `fi`, and with one mask shared by both
     * the `for` body read a nested `; fi` as the end of the loop. */
    unsigned      outer = p->closers;

    p->closers = CLOSE_FI | CLOSE_DONE | CLOSE_ESAC | CLOSE_BRACE | CLOSE_PAREN |
                 CLOSE_INPUT;

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

        /* The separators are skipped *only* when there is a `|` to continue the pipeline
         * with, and not otherwise.
         *
         * They used to be skipped unconditionally, which took the `;` out of `echo a; echo
         * b` before `parse_and_or()` ever saw it: the separator that joins two commands was
         * eaten by the parser of the *first* one, so `parse_and_or()` looked at a `T_WORD`
         * where it expected an operator, returned after one command, and `parse_program()`
         * reported the rest as trailing garbage.  Every `;`-separated pair in a script
         * failed, which is to say the most ordinary thing a shell does.
         *
         * `skip_separators()` is still what runs before the `|`, because `a ; | b` is not a
         * pipeline and `a | b` has to get past its blanks. */
        /* Look *past* any separators for a `|` without consuming them.
         *
         * Looking and consuming have to be separated, and that is the whole of this fix.
         * The separators belong to whoever is joining commands -- `parse_and_or()` reads
         * the `;` that joins two commands -- and a parser that consumes them on the way
         * out of the first command takes the operator before its caller can see it.  So
         * this peeks, and consumes only when the thing it found really is a pipe. */
        {
            int look = p->at;

            while (look < p->t->n &&
                   (p->t->v[look].kind == T_SEMI || p->t->v[look].kind == T_NEWLINE))
                look++;

            if (look >= p->t->n || p->t->v[look].kind != T_PIPE)
                break;

            p->at = look;
        }

        p->at++;
        skip_separators(p);

        /* Past the `|`, so a stage must follow: `a |` is a syntax error on every shell and
         * parsing it as `a` would run a command the user did not ask for. */
        if (at_closer(p) || peek(p)->kind == T_EOF) {
            fail(p, "a command was expected after `|'");
            cmd_free(first);
            cmd_free(pipe);
            return NULL;
        }

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

    p->closers = outer;

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

        /* A `;` or a newline that ends the list, rather than joining to another command.
         *
         * The distinction is the end of the input and a word that closes something: `fi`,
         * `done`, `esac`, `}`, `)`.  `while ...; do n=$((n+1)); echo "w=$n"; done` ends its
         * body with `echo "w=$n";` and the `;` there is a separator with nothing after it,
         * so treating it as a join asks for a command that is not there and the whole line
         * is reported as unparseable -- which is what it did for every loop, every `if` and
         * every `case` in the test script.
         *
         * A *joined* operator is a different matter and stays an error: `true &&` names an
         * operator that means "and" and promises a right-hand side, and quietly ignoring
         * it would run a command the user did not ask for. */
        if (at_closer(p)) {
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
    }

    return left;
}

cmd_t *parse_program(tokens_t *toks, const char **err) {
    /* `closers` starts as `CLOSE_INPUT`: at the top level the only thing that ends a list
     * is the end of the input, so a stray `fi` or `done` is trailing garbage and is
     * reported as such rather than silently ending a command. */
    parser_t p = { toks, 0, NULL, CLOSE_INPUT };
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
        /* The token that is left, named.
         *
         * "Something after the end of the command" says where the parser stopped and not
         * what it found there, and those are different questions: the first is answered by
         * every construct that ends in a word, and the second is answered by one line of a
         * script.  With the word in the message, `if C; then A; fi extra` says `extra`, and
         * so does `echo a;;`.
         *
         * It is built in a static buffer rather than malloc'd because `*err` is a `const
         * char *` the caller does not own or free -- the same contract every other message
         * here already has, and `failf()` above has the same reason. */
        static char buf[192];

        snprintf(buf, sizeof(buf), "something after the end of the command: `%s'",
                 peek(&p)->text != NULL ? peek(&p)->text : "(end of input)");

        cmd_free(c);
        *err = buf;
        return NULL;
    }

    return c;
}
