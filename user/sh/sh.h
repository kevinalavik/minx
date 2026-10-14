/* minx - `sh`, and the things it needs to talk about.
 *
 * A shell is four programs in one: a lexer, a parser, an expander and an executor, plus a
 * table of builtins.  They are separate here because they have genuinely different jobs
 * and because the boundaries are what keep each of them small.
 *
 *   lex.c     text -> tokens.  Knows about quoting and nothing else.
 *   parse.c   tokens -> a tree.  Knows about grammar and nothing else.
 *   expand.c  a word -> fields.  Knows about variables, globs and splitting.
 *   exec.c    a tree -> processes, pipes and builtins.
 *   builtins.c the commands that are not programs.
 *   var.c     variables, aliases and functions.
 *   main.c    the loop: read a line, parse it, run it, print `$?`.
 *
 * The one rule that shapes all of them: **nothing is expanded until it is about to be
 * used**.  A parser that expands as it parses cannot implement `for f in *`, because it
 * would have globbed the word list while the loop was still being read.  So words are kept
 * as they were typed -- quotes and all -- and `expand.c` is called by `exec.c` at the one
 * moment the text turns into arguments.
 */
#ifndef MINX_SH_H
#define MINX_SH_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* --- the tree ---------------------------------------------------------------- */

/* Forward, because the variable table below declares `func_get()` and the tree is defined
 * above it -- and a `typedef struct cmd cmd_t;` twice is a redefinition in C11. */
typedef struct cmd cmd_t;

/* What a command is.
 *
 * An enum rather than a set of node structs because every node is the same struct and a
 * shell's nodes are small; one struct with all the fields costs a few words per node and
 * saves every `->` from being a union discriminant.  The nodes are freed as soon as the
 * command they belong to has finished, so the peak count is one command's worth. */
typedef enum {
    CMD_SIMPLE,    /* words, redirections, and optional NAME=VALUE prefixes */
    CMD_LIST,      /* `a` then `b`, joined by ; & && || or a newline */
    CMD_PIPE,      /* `a` | `b` | `c` */
    CMD_SUB,       /* ( list ) -- a subshell */
    CMD_GROUP,     /* { list ; } -- a group, in this shell */
    CMD_IF,        /* if/elif/else */
    CMD_FOR,       /* for NAME in words ; do list ; done */
    CMD_WHILE,     /* while/until list do list done */
    CMD_CASE,      /* case WORD in pat) list ;; ... esac */
    CMD_FUNC       /* NAME ( ) { list } */
} cmd_kind_t;

/* What a redirection does.  The numbers are the file descriptor each one ends up on, which
 * is what a shell actually has to know: `2>&1` is "descriptor 2 becomes descriptor 1",
 * and `>&2` is "descriptor 1 becomes descriptor 2", and they are not the same operator
 * however similar they look. */
typedef enum {
    RD_IN,         /* <  file      -> fd 0 */
    RD_OUT,        /* >  file      -> fd 1 */
    RD_APPEND,     /* >> file      -> fd 1 */
    RD_OUT2,       /* >| file      -> fd 1, and refuse to clobber a directory */
    RD_IN2,        /* <& fd        -> fd 0, from another descriptor */
    RD_OUT2FD,     /* >& fd        -> fd 1, from another descriptor */
    RD_ERR,        /* 2> file      -> fd 2 */
    RD_ERRAPPEND,  /* 2>> file     -> fd 2 */
    RD_ERRTOFD,    /* 2>&1         -> fd 2, from another descriptor */
    RD_ALLIN       /* &> file      -> fd 1 and fd 2 */
} redir_kind_t;

typedef struct redir {
    redir_kind_t   kind;
    char          *word;      /* the file name, or the descriptor as text */
    struct redir  *next;
} redir_t;

/* A case arm: a `|`-separated list of patterns and the list to run for it. */
typedef struct case_arm {
    char **patterns;          /* NULL-terminated, each one a pattern */
    cmd_t *body;
    int    taken;             /* `;;` rather than `;&`, so the next arm is not tried */
} case_arm_t;

struct cmd {
    cmd_kind_t    kind;

    /* CMD_SIMPLE */
    char        **words;      /* NULL-terminated; the quotes are still in them */
    int           nwords;
    char        **assigns;    /* NAME=VALUE prefixes, before the words */
    int           nassigns;
    redir_t      *redirs;

    /* CMD_LIST, CMD_PIPE, CMD_SUB, CMD_GROUP, CMD_IF, CMD_FOR, CMD_WHILE, CMD_CASE,
     * CMD_FUNC */
    struct cmd   *a;          /* left, or the condition, or the body */
    struct cmd   *b;          /* right, or the else-branch */
    struct cmd  **stages;     /* the stages of a pipeline */
    int           nstages;
    case_arm_t   *arms;       /* case */
    int           narms;

    /* The reserved word, or the loop variable, or the function's name. */
    char         *name;

    /* `elif` chains.
     *
     * A `CMD_IF` holds *one* condition and one or two branches, so `elif` cannot be a
     * sibling of the `if` -- there is no second node.  Each `elif`'s condition and body are
     * one `CMD_LIST` pair named `elif`, and these link them in source order.  The executor
     * walks the chain and takes the first whose condition holds.
     *
     * It was written the other way round: each `elif` overwrote `c->a` and `c->b`, so a
     * chain kept only its last branch.  `if true; then A; elif true; then B; fi` ran B,
     * which is wrong in the direction that looks like the shell working: the *first*
     * matching branch is the one that runs, and a chain whose first branch is unreachable
     * is a chain with the wrong branch. */
    struct cmd  *elifs;
    struct cmd  *next_elif;

    /* The `else` branch, for `CMD_IF`.
     *
     * Its own field because `a` is the condition and `b` is the branch that runs when the
     * condition *holds* -- `if C; then A; else B; fi` runs `A` when `C` is true and `B`
     * when it is false, and with `a` and `b` both spoken for there is nowhere else for `B`
     * to go.  Putting it in `b` ran the else-branch whenever the condition was true. */
    struct cmd  *orelse;

    /* `until` rather than `while`. */
    int           negated;

    /* `for x in` with no `in`, which iterates over the positional parameters. */
    int           no_in;
};

/* A parsed command as an indented tree.
 *
 * Accept-or-reject is one bit per line, and it cannot tell a right tree from a wrong one --
 * which is how `if true then A elif true then B fi` came to parse cleanly and run B.  Anything
 * that tests this parser has to be able to *look* at what it built, and that is all this is.
 *
 * `tools/sh-parser-test.sh` uses it to compare the words in the tree against the words on
 * the line; the guest uses it to print the tree of a line it could not parse, so a script
 * that does not work shows what the parser made of it and not only what it complained about.
 * It prints with `printf()` so that the same source serves both: the libc has it in the
 * guest, and the host test compiles `parse.c` unmodified. */
void cmd_tree_dump(cmd_t *c, int depth);

/* --- tokens ----------------------------------------------------------------- */

typedef enum {
    T_EOF = 0,
    T_WORD,
    T_ASSIGN,       /* NAME=VALUE with a valid name -- only a word at the start of a
                     * command, which is the only place it means anything */
    T_NEWLINE,
    T_SEMI,         /* ; or &, with `background` saying which */
    T_AND_IF,       /* && */
    T_OR_IF,        /* || */
    T_PIPE,
    T_LPAREN,       /* ( */
    T_RPAREN,       /* ) */
    T_LBRACE,       /* { */
    T_RBRACE,       /* } */
    T_DSEMI,        /* ;; */
    T_REDIR,        /* < > >> <& >& >| &> and the 2> forms, with `kind` */
    T_DPAREN,       /* $( -- which becomes a word once its contents are parsed */
    T_BACKTICK,     /* ` -- the other spelling of the same thing */
    T_IF, T_THEN, T_ELIF, T_ELSE, T_FI,
    T_FOR, T_IN, T_DO, T_DONE,
    T_WHILE, T_UNTIL, T_CASE, T_ESAC,
    T_BANG          /* ! -- reserved, but only as a word */
} tok_kind_t;

typedef struct {
    tok_kind_t kind;

    /* For T_WORD and T_ASSIGN: the text with its quotes still in it.  Kept verbatim
     * because expansion is a separate pass and a parser that ate the quotes would have
     * to remember what they were. */
    char      *text;

    /* For T_REDIR. */
    redir_kind_t rkind;

    /* For T_WORD: is this the descriptor the lexer prefixed to a redirection, as in the `2`
     * of `2>file`?
     *
     * It has to be a flag rather than something the parser works out, because the token is
     * a one-digit word either way: the `2` of `2>file` and the `2` of `>&2` are the same
     * shape, and only the lexer knows which one it produced.  Guessing from the text made
     * `>&2` into `>&` with no argument, which `collect_redirs()` read as descriptor zero --
     * so `echo hi >&2` sent its output to the command's *input* and printed nothing. */
    int       fd_prefixed;

    /* For T_SEMI: an `&` rather than a `;`. */
    int       background;
} token_t;

typedef struct {
    token_t *v;
    int      n;
    int      cap;
} tokens_t;

/* lex.c */
int   lex_line(const char *line, tokens_t *out);
void  tokens_free(tokens_t *t);

/* parse.c -- returns NULL and sets `*err` on a syntax error. */
cmd_t *parse_program(tokens_t *toks, const char **err);
void  cmd_free(cmd_t *c);

/* --- variables -------------------------------------------------------------- */

/* `env.c`'s table.  Exported and not are different questions and one list cannot answer
 * both: `sh` sets a dozen variables that no child should see, and `environ` is what a
 * child sees. */
const char *var_get(const char *name);
void        var_set(const char *name, const char *value, int exported);
void        var_unset(const char *name);
int         var_exported(const char *name);
char      **var_environ(void);
void        var_export_all(void);
void        var_import_environ(char **envp);

/* aliases and functions, in the same shape because a shell treats them alike */
const char *alias_get(const char *name);
void        alias_set(const char *name, const char *value);
void        alias_unset(const char *name);

cmd_t      *func_get(const char *name);
void        func_set(const char *name, cmd_t *body);

/* --- expansion -------------------------------------------------------------- */

/* One word as typed, expanded into the fields it stands for.
 *
 * `out` is filled with malloc'd strings and the count returned; the caller frees the list
 * with `free_fields()`.  `assign` is non-NULL when the word is the right-hand side of an
 * assignment, and then the result is one string however much it looks like several --
 * because `x=`$(ls)` is one value and splitting it would be a different language. */
int   expand_word(const char *word, char ***out);
int   expand_assign(const char *word, char **out);
char *expand_to_string(const char *word);

/* Builtins. */
typedef struct {
    const char *name;
    int  (*fn)(int argc, char **argv);
    int   needs_subshell;   /* the builtin must not run in the shell's own process */
} builtin_t;

const builtin_t *builtin_find(const char *name);
int             builtin_count(void);
const builtin_t *builtin_at(int i);

/* The shell's own state, which the builtins and the executor both need. */
typedef struct {
    char  **argv;       /* the positional parameters, $1.. */
    int     argc;
    char   *argv0;
    int     status;     /* $? */
    int     exit_code;  /* set by the `exit` builtin */
    int     exiting;
    int     last_bg;    /* the pid of the last background job */
    const char *prompt; /* PS1, resolved once at startup */
} sh_state_t;

extern sh_state_t sh;

/* The name of the interactive shell, for prompts and error messages. */
extern const char *sh_name;

/* `exit` sets these; `exec.c` checks them after every command. */
void sh_exit(int code);

/* --- loop and function control ------------------------------------------------- */

/* `break [n]`, `continue [n]` and `return [n]`, from `builtins.c`.
 *
 * Each counts how many loops or function calls it still has to unwind rather than setting a
 * flag, because `break 2` and `return 2` exist and a flag cannot say "two".  `exec.c`
 * decrements the count as each construct it passes unwinds, and checks it before running
 * every command so that nothing after the `break` in the same list runs.
 *
 * `levels` of zero means one, which is what a bare `break` is: a shell that treated
 * `break 0` as "break nothing" would let `break $n` with an unset `$n` silently do the
 * wrong thing instead of doing the obvious one. */
void sh_break(int levels);
void sh_continue(int levels);
void sh_return(int levels);
void sh_return_status(int status);

/* Is any of the three in effect?  The dispatcher asks before every command. */
int  sh_loop_control_pending(void);

/* Did a `return` just reach the outermost function call?  Consumes one level. */
int  sh_return_pending(void);

/* Write a diagnostic in the shell's own format.  All of them go to stderr, because a
 * diagnostic on stdout is a line in somebody's pipeline. */
void sh_error(const char *fmt, ...);
void sh_perror(const char *what);

/* --- from exec.c ------------------------------------------------------------- */

/* Run one node of the tree.  The executor's whole interface. */
void exec_cmd(cmd_t *c, int *status);

/* Parse and run a string of shell code in *this* process.  `eval` and `.` use it, and so
 * does `sh -c`, and the difference from running it in a child is the whole reason those two
 * are builtins: a `cd` in an `eval` has to move this shell. */
void exec_run_string(const char *text, int *status);

/* The same, for a file.  Returns 127 for "no such file", which is what a shell does for a
 * command it cannot find, because `.` on a missing file is a missing command. */
void exec_run_string_file(const char *path, int *status);

/* Find `name` on PATH, returning a one-element vector holding the path, or NULL.
 *
 * Returns a *vector* rather than a string because the caller has to free something and
 * because `type` prints the path it found and `execvp` passes it on -- two callers, two
 * different uses, one search. */
char **path_search(const char *name);

/* Run a command line and give back its standard output, with the trailing newlines left on.
 * The command substitution's job; returns NULL if the command could not be run at all. */
char *sh_run_capture(const char *text);

/* --- from jobs.c -------------------------------------------------------------- */

/* `wait` with a job specification: a pid, `%1`, `%+` or `%`.  `-1` means every job, and is
 * what `wait` with no arguments asks for. */
int job_wait(int spec, int *status);

/* `jobs`: print the table. */
void job_list(void);

/* `fg` and `bg`. */
void job_foreground(const char *spec, int *status);
void job_background(const char *spec);

/* Turn `1234`, `%1`, `%+` or `%` into a pid, or -1.  Used by `kill`, which is why it takes
 * a job specification and not just a number. */
pid_t job_resolve(const char *spec);

/* --- from main.c -------------------------------------------------------------- */

void history_print(void);

/* `kill -NAME`: a signal name to a number.  Unknown names are SIGTERM, which is what every
 * shell does -- a typo in a signal name must not silently send nothing. */
int signal_number(const char *name);

/* Read one line, or return -1 at end of input with nothing read.  A character at a time
 * rather than with `fgets`, because a line can arrive without a newline and `fgets` would
 * hand back half a command. */
int sh_read_line(FILE *in, char **out);

/* Has the shell been told to exit, and with what? */
int sh_exiting(void);
int sh_exit_code(void);

/* Are we inside a background command?  The `wait` builtin and the terminal's ^C handling
 * both need to know and neither can tell from the tree. */
void sh_note_background(int on);

/* `fnmatch` for the `case` statement, where a pattern with quotes in it is a string and
 * not a pattern. */
int  fnmatch_pattern(const char *pattern, const char *text);

/* The `read` builtin records whether it stopped at end of file; `got_eof()` reports it. */
void sh_set_read_eof(int yes);

/* Did the last `read` from `fd` stop at end of file rather than at a newline?  The
 * distinction is what makes `while read line; do ...; done < file` stop. */
int got_eof(int fd);

/* --- from expand.c ------------------------------------------------------------ */

long sh_arith(const char *text);

#endif /* MINX_SH_H */
