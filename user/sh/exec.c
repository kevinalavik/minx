/* minx - the executor.
 *
 * A tree into processes, pipes and builtin calls.  This is the only file here that knows
 * what a process is, and the only one that calls `fork()`.
 *
 * Three rules shape it:
 *
 *   - **A fork happens as late as possible.**  A shell that forks for a compound command
 *     forks for things that change nothing -- a `{ }` group, a variable assignment, a
 *     builtin with no side effects the parent can see.  Each of those is a fork a person
 *     pays for and cannot explain, and `for i in 1 2 3; do x=1; done` in a loop is the
 *     difference between a shell that feels instant and one that does not.
 *
 *   - **`exec` replaces; everything else waits.**  A command whose status is not asked for
 *     runs in a child and is not waited for.  That is the whole of `&`, and it is why
 *     `sleep 100 &` returns immediately and `sleep 100` does not.
 *
 *   - **Redirections are applied to the child, in order, before the `exec`.**  After the
 *     fork and never before it, so that `cmd > file` does not send the shell's own output
 *     into the file for the half a microsecond before the child starts.
 *
 * The one piece of state this shares with the rest of the shell is `sh.status`, and it is
 * updated in exactly one place per command so that `$?` is never the status of something
 * other than the last thing that ran.
 */
#include "sh.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

sh_state_t sh;
const char *sh_name = "sh";

/* Run one node of the tree.  Declared here because `run_simple()` calls it for a function
 * body and the definition is below; it is also the executor's whole interface, so it is
 * declared in sh.h as well. */

/* How many entries a NULL-terminated vector has, not counting the NULL. */
static int args_n(char **v);

/* A growable NULL-terminated vector of strings.
 *
 * Duplicated here rather than exported from expand.c because the expander's is an
 * implementation detail of the *word* walk -- it exists to collect the fields of one word,
 * and its `sbuf_t` alongside it is what makes splitting possible.  The executor's list is
 * a list of words, and a second one is cheaper than a shared abstraction over two different
 * things. */
typedef struct {
    char **v;
    int    n;
    int    cap;
} vec_t;

static void vec_add(vec_t *f, const char *s) {
    if (f->n + 2 > f->cap) {
        int want = f->cap ? f->cap * 2 : 8;
        char **grown = realloc(f->v, (size_t)want * sizeof(char *));

        if (grown == NULL)
            return;

        f->v = grown;
        f->cap = want;
    }

    f->v[f->n] = strdup(s != NULL ? s : "");
    f->v[f->n + 1] = NULL;
    f->n++;
}

/* Set by the `exit` builtin and by a signal the shell decides to die of. */
static int   exiting;
static int   exit_code;

/* Whether the command being run is a background one, which the `wait` and `jobs` builtins
 * and the terminal's SIGINT handling all need to know and none of which can be told by
 * looking at the tree from outside. */
static int   in_background;

/* Where job control's table lives, in jobs.c. */
void job_note_started(pid_t pid, const char *name);
void job_note_finished(pid_t pid, int status);
void job_before_wait(void);
void job_after_wait(void);

void sh_exit(int code) {
    exiting = 1;
    exit_code = code;
    sh.exiting = 1;
    sh.exit_code = code;
}

void sh_error(const char *fmt, ...) {
    va_list ap;

    fflush(stdout);
    fprintf(stderr, "%s: ", sh_name);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
}

void sh_perror(const char *what) {
    sh_error("%s: %s", what, strerror(errno));
}

/* --- PATH ----------------------------------------------------------------------- */

/* The directories searched for a command with no slash in its name.
 *
 * `PATH` is read once per search rather than cached, because a script that sets PATH and
 * then runs a command expects the new one, and because the cost is one `getenv`. */
static const char *path_value(void) {
    const char *p = var_get("PATH");

    if (p == NULL)
        p = "/bin:/usr/bin";

    return p;
}

/* `name` has a slash: it is a path, and PATH has nothing to say about it. */
static int has_slash(const char *name) {
    return strchr(name, '/') != NULL;
}

static int is_executable_file(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;

    if (!S_ISREG(st.st_mode))
        return 0;

    /* Root may run anything; everybody else needs the execute bit, and the check is on the
     * file's own mode rather than on its owner because there is one user. */
    if (access(path, X_OK) != 0)
        return 0;

    return 1;
}

char **path_search(const char *name) {
    if (name == NULL || name[0] == 0)
        return NULL;

    if (has_slash(name)) {
        char **v = malloc(sizeof(char *));

        if (v == NULL)
            return NULL;

        v[0] = strdup(name);
        v[1] = NULL;
        return v;
    }

    const char *path = path_value();
    size_t      len = strlen(path);
    char       *copy = malloc(len + 1);

    if (copy == NULL)
        return NULL;

    memcpy(copy, path, len + 1);

    const char *p = copy;

    while (*p != 0) {
        const char *colon = strchr(p, ':');
        size_t      dlen = colon != NULL ? (size_t)(colon - p) : strlen(p);

        /* An empty element means the current directory, and it means it on every Unix.  It
         * is also the reason `.` in PATH is a security problem on a system with more than
         * one user; here there is one, and refusing it would break scripts. */
        if (dlen == 0) {
            char *cand = malloc(strlen(name) + 3);

            if (cand == NULL)
                break;

            sprintf(cand, "./%s", name);

            if (is_executable_file(cand)) {
                free(copy);

                char **v = malloc(sizeof(char *));

                if (v == NULL) {
                    free(cand);
                    return NULL;
                }

                v[0] = cand;
                v[1] = NULL;
                return v;
            }

            free(cand);
        } else {
            char *cand = malloc(dlen + strlen(name) + 2);

            if (cand == NULL)
                break;

            memcpy(cand, p, dlen);
            cand[dlen] = '/';
            strcpy(cand + dlen + 1, name);

            if (is_executable_file(cand)) {
                free(copy);

                char **v = malloc(sizeof(char *));

                if (v == NULL) {
                    free(cand);
                    return NULL;
                }

                v[0] = cand;
                v[1] = NULL;
                return v;
            }

            free(cand);
        }

        if (colon == NULL)
            break;

        p = colon + 1;
    }

    free(copy);
    return NULL;
}

/* --- redirections ----------------------------------------------------------------- */

/* Open one redirection and return the descriptor it should be *dup2'd* to, or -1.
 *
 * Split from applying it because the two halves happen at different times: the *decision*
 * has to be made in the child, before `exec`, while the redirections of a whole command
 * are collected as a list.  `dup2` into 0/1/2 has to happen in the child, after the fork,
 * or the shell's own standard input would be gone. */
typedef struct {
    int  from;        /* the descriptor this redirection wants to become */
    int  to;          /* the descriptor it becomes, or -1 to close */
    char *path;       /* the file, for the file redirections */
    int   flags;
} open_redir_t;

/* Build the list of open redirections for a command.
 *
 * Returns the count, or -1 on a failure the caller reports.  The `to` of a descriptor-to-
 * descriptor redirection is computed here rather than in the child because it depends only
 * on the word, and doing it in the child would mean a syntax mistake in `2>&9` produced a
 * diagnostic from a process nobody is waiting for. */
static int collect_redirs(const redir_t *r, open_redir_t *out, int max) {
    int n = 0;

    for (; r != NULL && n < max; r = r->next) {
        open_redir_t *o = &out[n];

        switch (r->kind) {
        case RD_IN:
            o->from = -1;
            o->to = 0;
            o->path = r->word;
            o->flags = O_RDONLY;
            break;

        case RD_OUT:
        case RD_OUT2:
            o->from = -1;
            o->to = 1;
            o->path = r->word;
            o->flags = O_WRONLY | O_CREAT | (r->kind == RD_OUT2 ? 0 : O_TRUNC);
            break;

        case RD_APPEND:
            o->from = -1;
            o->to = 1;
            o->path = r->word;
            o->flags = O_WRONLY | O_CREAT | O_APPEND;
            break;

        case RD_ERR:
            o->from = -1;
            o->to = 2;
            o->path = r->word;
            o->flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;

        case RD_ERRAPPEND:
            o->from = -1;
            o->to = 2;
            o->path = r->word;
            o->flags = O_WRONLY | O_CREAT | O_APPEND;
            break;

        case RD_IN2:
        case RD_OUT2FD:
        case RD_ERRTOFD: {
            char *end;
            long  v = strtol(r->word, &end, 10);

            if (*end != 0 || v < 0 || v > 255) {
                sh_error("%s: %s: not a file descriptor", r->word, "redirect");
                return -1;
            }

            o->from = (int)v;
            o->to = (r->kind == RD_IN2) ? 0 : (r->kind == RD_ERRTOFD ? 2 : 1);
            o->path = NULL;
            o->flags = 0;
            break;
        }

        case RD_ALLIN:
            /* `&>` is two redirections in one operator, because POSIX added it after
             * `2>&1` already existed and it is shorter to type. */
            o->from = -1;
            o->to = 1;
            o->path = r->word;
            o->flags = O_WRONLY | O_CREAT | O_TRUNC;
            out[n + 1] = *o;
            out[n + 1].to = 2;
            n++;
            break;
        }
    }

    return n;
}

/* Apply them, in order, in the current process.
 *
 * Left to right, and that is not tidiness: `> a > b` means the second one wins, and
 * `> file 2>&1` means "the file, then *its* descriptor 1 becomes descriptor 2" -- which is
 * what makes `cmd > f 2>&1` capture the error and `cmd 2>&1 > f` not.  The difference
 * between those two is one of the oldest things a shell user knows and it is decided here,
 * by applying in the order written. */
static int apply_redirs(const open_redir_t *v, int n) {
    for (int i = 0; i < n; i++) {
        int fd;

        if (v[i].path != NULL) {
            char *path = expand_to_string(v[i].path);

            fd = open(path, v[i].flags, 0666);
            free(path);

            if (fd < 0) {
                sh_perror(v[i].path);
                return -1;
            }
        } else {
            fd = v[i].from;

            if (fd < 0) {
                sh_error("%d: Bad file descriptor", v[i].from);
                return -1;
            }
        }

        if (v[i].to >= 0 && fd != v[i].to) {
            if (dup2(fd, v[i].to) < 0) {
                sh_perror("dup2");
                return -1;
            }

            if (fd > 2)
                close(fd);
        }
    }

    return 0;
}

/* --- the command vector ---------------------------------------------------------- */

static void free_fields(char **v) {
    if (v == NULL)
        return;

    for (int i = 0; v[i] != NULL; i++)
        free(v[i]);

    free(v);
}

/* Expand a simple command's words and assignments.
 *
 * Returns the argument vector, or NULL with `*status` set.  An empty result is *not* an
 * error: `> file` and `x=1` are commands with nothing to run, and they succeed. */
static char **expand_command(const cmd_t *c, int *status) {
    vec_t args;
    int   i = 1;

    memset(&args, 0, sizeof(args));

    /* Assignments first, because `VAR=x command` sets it for the command and a word after
     * an assignment is a word, not an assignment. */
    for (int k = 0; k < c->nassigns; k++) {
        char *value = NULL;

        expand_assign(c->assigns[k], &value);

        char *eq = strchr(c->assigns[k], '=');

        if (eq != NULL && value != NULL) {
            char *name = malloc((size_t)(eq - c->assigns[k]) + 1);

            if (name != NULL) {
                memcpy(name, c->assigns[k], (size_t)(eq - c->assigns[k]));
                name[eq - c->assigns[k]] = 0;

                if (var_exported(name))
                    var_set(name, value, 1);
                else
                    var_set(name, value, 0);

                free(name);
            }
        }

        free(value);
    }

    for (; i <= c->nwords; i++) {
        char **one = NULL;
        int    n = expand_word(c->words[i - 1], &one);

        for (int k = 0; k < n; k++)
            vec_add(&args, one != NULL ? one[k] : "");

        free_fields(one);
    }

    /* A command with no words but with assignments still runs something: a shell.  This is
     * what makes `PATH=/bin ls` work when `ls` is not on the path it had, and what makes a
     * typo'd assignment run a second shell instead of doing nothing. */
    if (args.n == 0) {
        if (c->nassigns > 0) {
            char  *argv[2];
            char  *env[1];
            char **e = var_environ();

            (void)env;
            argv[0] = (char *)sh_name;
            argv[1] = NULL;

            pid_t pid = fork();

            if (pid == 0) {
                execve("/bin/sh", argv, e);
                _exit(127);
            }

            if (pid < 0) {
                sh_perror("fork");
                *status = 1;
                return NULL;
            }

            int st = 0;

            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;

            *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            return NULL;
        }

        *status = 0;
        return NULL;
    }

    *status = 0;
    return args.v;
}

/* --- running one command ----------------------------------------------------------- */

/* Wait for a child and turn its status into a shell status.
 *
 * The conversions are the ones POSIX fixes: an exit status is the low eight bits, and a
 * death by signal is 128 plus the signal.  So `sh -c 'kill -9 $$'` reports 137 and a script
 * that tests `$? -gt 128` to mean "it died" is right. */
static int reap(pid_t pid) {
    int st = 0;

    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR)
            continue;

        sh_perror("waitpid");
        return 1;
    }

    job_note_finished(pid, st);

    if (WIFEXITED(st))
        return WEXITSTATUS(st);

    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);

    return 1;
}

/* Run a simple command, forking only when it has to.
 *
 * The three cases that do *not* fork:
 *
 *   - a builtin that does not change the shell, which is every builtin except `exit` and
 *     `exec`.  Forking for `cd` would make `cd ..` a no-op in the parent, which is the
 *     single most-reported shell bug there is.
 *   - a command whose status nobody asked for, which is every command in a `&` or in a
 *     condition that is not a condition.
 *   - a function, which is a tree of this shell's own nodes and has no program to run.
 */
static int run_simple(cmd_t *c, int want_status) {
    int    status = 0;
    char **args = expand_command(c, &status);

    if (args == NULL) {
        /* An assignment-only command, or a failure while expanding.  The redirections still
         * have to happen: `> file` alone is a command, and it is how a script truncates a
         * file. */
        if (status == 0 && c->redirs != NULL) {
            open_redir_t r[16];
            int          n = collect_redirs(c->redirs, r, 15);

            if (n > 0) {
                pid_t pid = fork();

                if (pid == 0) {
                    int bad = apply_redirs(r, n) < 0;

                    _exit(bad ? 1 : 0);
                }

                if (pid < 0) {
                    sh_perror("fork");
                    return 1;
                }

                if (want_status)
                    return reap(pid);

                job_note_started(pid, "redirection");
                job_before_wait();
                job_after_wait();
                return 0;
            }
        }

        return status;
    }

    /* A function. */
    cmd_t *body = func_get(args[0]);

    if (body != NULL) {
        int    saved_argc = sh.argc;
        char **saved_argv = sh.argv;
        int    fstatus = 0;

        /* The function's parameters are its arguments, and they come back afterwards
         * untouched -- `set --` inside a function must not change the caller's. */
        sh.argc = args_n(args);
        sh.argv = args + 1;

        exec_cmd(body, &fstatus);

        sh.argc = saved_argc;
        sh.argv = saved_argv;
        free_fields(args);

        return fstatus;
    }

    const builtin_t *bi = builtin_find(args[0]);

    if (bi != NULL) {
        open_redir_t r[16];
        int          n = c->redirs != NULL ? collect_redirs(c->redirs, r, 15) : 0;

        if (n < 0) {
            free_fields(args);
            return 1;
        }

        /* `cd > file` has to happen in a child, or the shell's own descriptor 1 would be
         * replaced by a file for the rest of the session. */
        if (n > 0) {
            pid_t pid = fork();

            if (pid == 0) {
                int bad = apply_redirs(r, n) < 0;
                int rc = bad ? 1 : bi->fn(args_n(args), args);

                fflush(stdout);
                _exit(rc);
            }

            free_fields(args);

            if (pid < 0) {
                sh_perror("fork");
                return 1;
            }

            return want_status ? reap(pid) : 0;
        }

        int rc = bi->fn(args_n(args), args);

        free_fields(args);
        return rc;
    }

    /* A program. */
    char **path = path_search(args[0]);

    if (path == NULL) {
        sh_error("%s: not found", args[0]);
        free_fields(args);
        return 127;
    }

    open_redir_t r[16];
    int          n = c->redirs != NULL ? collect_redirs(c->redirs, r, 15) : 0;

    if (n < 0) {
        free(path[0]);
        free(path);
        free_fields(args);
        return 1;
    }

    if (!want_status) {
        /* Background: no fork needed if the command is a builtin, and no `reap` if it is
         * not -- the shell has to be able to carry on typing while it runs. */
        pid_t pid = fork();

        if (pid == 0) {
            signal(SIGINT, SIG_IGN);

            if (apply_redirs(r, n) < 0)
                _exit(1);

            char **env = var_environ();

            execve(path[0], args, env);
            _exit(127);
        }

        if (pid < 0) {
            sh_perror("fork");
            free(path[0]);
            free(path);
            free_fields(args);
            return 1;
        }

        sh.last_bg = (int)pid;
        job_note_started(pid, args[0]);
        free(path[0]);
        free(path);
        free_fields(args);
        return 0;
    }

    pid_t pid = fork();

    if (pid == 0) {
        if (apply_redirs(r, n) < 0)
            _exit(1);

        char **env = var_environ();

        execve(path[0], args, env);

        /* Only reached when the exec failed.  The message goes to stderr, which by now is
         * whatever the redirections made it -- so `cmd > out` does not put its own "not
         * found" into `out`, and `cmd 2>&1 > out` does.  That asymmetry is correct and is
         * exactly what the user asked for. */
        fprintf(stderr, "%s: %s\n", args[0], strerror(errno));
        _exit(127);
    }

    if (pid < 0) {
        sh_perror("fork");
        free(path[0]);
        free(path);
        free_fields(args);
        return 1;
    }

    int rc = reap(pid);

    free(path[0]);
    free(path);
    free_fields(args);
    return rc;
}

/* The definition, next to the forward declaration rather than beside its first user. */
static int args_n(char **v) {
    int n = 0;

    while (v != NULL && v[n] != NULL)
        n++;

    return n;
}

/* --- command substitution ------------------------------------------------------------ */

/* Run a command line and collect its standard output.
 *
 * The pipe is read *before* the child is waited for, and that ordering is not an
 * optimisation.  A child that writes more than a pipe buffer -- `$(ls /usr/bin)`, or any
 * command with a `for` loop printing a line per iteration -- blocks on the write, and a
 * shell that waited first would wait for ever.  Reading to end-of-file first is what lets
 * the child finish. */
char *sh_run_capture(const char *text) {
    int fds[2];

    if (pipe(fds) != 0) {
        sh_perror("pipe");
        return NULL;
    }

    pid_t pid = fork();

    if (pid < 0) {
        sh_perror("fork");
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);

        /* The child's standard error stays where it is: `$(ls /nowhere)` has to say why on
         * the terminal, not into the variable. */
        int status = 0;

        exec_run_string(text, &status);
        fflush(stdout);
        _exit(status);
    }

    close(fds[1]);

    size_t cap = 256;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (buf == NULL) {
        close(fds[0]);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;

        return NULL;
    }

    for (;;) {
        if (len + 64 >= cap) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL)
                break;

            buf = grown;
            cap *= 2;
        }

        ssize_t n = read(fds[0], buf + len, cap - len - 1);

        if (n <= 0)
            break;

        len += (size_t)n;
    }

    buf[len] = 0;
    close(fds[0]);

    int st = 0;

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;

    return buf;
}

/* --- the tree ------------------------------------------------------------------------- */

/* Run one node.  `want_status` is false for anything whose status nobody is going to look
 * at: the left of `&&` when the right is going to run anyway, a background command, and the
 * body of a loop.  Skipping the wait there is the difference between a shell that responds
 * immediately and one that does not. */
void exec_cmd(cmd_t *c, int *status) {
    int status_here = 0;

    if (c == NULL) {
        if (status != NULL)
            *status = 0;

        return;
    }

    switch (c->kind) {
    case CMD_SIMPLE:
        status_here = run_simple(c, status != NULL || !in_background);

        if (status != NULL)
            *status = status_here;

        break;

    case CMD_LIST: {
        const char *op = c->name != NULL ? c->name : ";";

        if (strcmp(op, "&&") == 0) {
            int left = 0;

            exec_cmd(c->a, &left);

            if (left == 0)
                exec_cmd(c->b, status);
            else if (status != NULL)
                *status = left;
        } else if (strcmp(op, "||") == 0) {
            int left = 0;

            exec_cmd(c->a, &left);

            if (left != 0)
                exec_cmd(c->b, status);
            else if (status != NULL)
                *status = left;
        } else if (strcmp(op, "&") == 0) {
            /* Background.  The `&` is the whole of the operator: the command runs in a
             * child, is not waited for, and its status is 0. */
            int saved = in_background;

            in_background = 1;
            exec_cmd(c->a, NULL);
            in_background = saved;

            if (status != NULL)
                *status = 0;
        } else {
            exec_cmd(c->a, status);
            exec_cmd(c->b, status);
        }

        break;
    }

    case CMD_PIPE: {
        /* Every stage but the last gets a pipe; the last inherits the command's own output.
         * The stages are forked left to right and the parent closes both ends of each pipe
         * it is not using -- a descriptor left open in the shell is what makes a pipeline
         * hang for ever, because the read end never sees end-of-file. */
        int n = c->nstages;

        if (n <= 0) {
            if (status != NULL)
                *status = 0;

            break;
        }

        int  infd = -1;
        pid_t pids[64];
        int    npids = 0;

        for (int i = 0; i < n && npids < 64; i++) {
            int fds[2] = { -1, -1 };

            int last = (i == n - 1);

            if (!last && pipe(fds) != 0) {
                sh_perror("pipe");
                break;
            }

            pid_t pid = fork();

            if (pid < 0) {
                sh_perror("fork");
                break;
            }

            if (pid == 0) {
                if (infd >= 0) {
                    dup2(infd, 0);
                    close(infd);
                }

                if (!last) {
                    close(fds[0]);
                    dup2(fds[1], 1);
                    close(fds[1]);
                }

                /* A stage's own redirections are applied after the pipe wiring, so that
                 * `cmd | other > file` sends `other`'s *output* to the file and not the
                 * pipe.  The other order would send the pipe into the file. */
                if (c->stages[i]->redirs != NULL) {
                    open_redir_t r[16];
                    int          rn = collect_redirs(c->stages[i]->redirs, r, 15);

                    if (rn < 0 || apply_redirs(r, rn) < 0)
                        _exit(1);
                }

                int st = 0;

                exec_cmd(c->stages[i], &st);
                fflush(stdout);
                _exit(st);
            }

            pids[npids++] = pid;

            if (infd >= 0)
                close(infd);

            if (!last) {
                close(fds[1]);
                infd = fds[0];
            }
        }

        if (infd >= 0)
            close(infd);

        int last_status = 0;

        for (int i = 0; i < npids; i++) {
            int st = reap(pids[i]);

            /* The pipeline's status is the *last* stage's, which is what `cmd | grep x`
             * tests: a shell that reported the first stage's status would make
             * `false | true` succeed. */
            if (i == npids - 1)
                last_status = st;
        }

        if (status != NULL)
            *status = last_status;

        break;
    }

    case CMD_SUB: {
        /* A subshell is a fork and nothing else.  There is no cheaper version, because
         * everything the body might do -- change a directory, set a variable, exit -- would
         * have to be undone afterwards. */
        pid_t pid = fork();

        if (pid < 0) {
            sh_perror("fork");

            if (status != NULL)
                *status = 1;

            break;
        }

        if (pid == 0) {
            int st = 0;

            exec_cmd(c->a, &st);

            if (c->redirs != NULL) {
                open_redir_t r[16];
                int          n = collect_redirs(c->redirs, r, 15);

                if (n > 0 && apply_redirs(r, n) < 0)
                    _exit(1);
            }

            fflush(stdout);
            _exit(st);
        }

        if (status != NULL)
            *status = reap(pid);
        else
            job_note_started(pid, "(subshell)");

        break;
    }

    case CMD_GROUP:
        exec_cmd(c->a, status);
        break;

    case CMD_IF: {
        /* `c->a` is the most recent condition and `c->b` its body.  The parser folded the
         * elif chain into pairs so that this is a loop with no state. */
        int cond = 0;

        exec_cmd(c->a, &cond);

        if (cond == 0)
            exec_cmd(c->b, status);
        else if (status != NULL)
            *status = 0;

        break;
    }

    case CMD_FOR: {
        vec_t items;
        int   st = 0;

        memset(&items, 0, sizeof(items));

        if (c->no_in) {
            /* `for x` with no `in` iterates over the positional parameters.  It is a rare
             * form and it is the one that makes `set -- a b c; for x; do echo $x; done`
             * work, which is how a shell script written for another shell sometimes starts. */
            for (int i = 0; i < sh.argc; i++) {
                char **one = NULL;

                expand_word(sh.argv[i], &one);

                for (int k = 0; one != NULL && one[k] != NULL; k++)
                    vec_add(&items, one[k]);

                free_fields(one);
            }
        } else {
            for (int i = 0; i < c->nwords; i++) {
                char **one = NULL;
                int    n = expand_word(c->words[i], &one);

                for (int k = 0; k < n; k++)
                    vec_add(&items, one[k]);

                free_fields(one);
            }
        }

        /* The list is expanded *once*, before the loop.  A shell that re-expanded it on
         * every iteration would see a different set of files each time round, which is
         * what `for f in *` in a loop that creates files has to avoid. */
        for (int i = 0; i < items.n; i++) {
            var_set(c->name, items.v[i], 0);
            exec_cmd(c->a, &st);

            if (exiting)
                break;
        }

        for (int i = 0; i < items.n; i++)
            free(items.v[i]);

        free(items.v);

        if (status != NULL)
            *status = st;

        break;
    }

    case CMD_WHILE: {
        int st = 0;

        for (;;) {
            int cond = 0;

            exec_cmd(c->a, &cond);

            if (exiting)
                break;

            int go = c->negated ? (cond != 0) : (cond == 0);

            if (!go)
                break;

            exec_cmd(c->b, &st);

            if (exiting)
                break;
        }

        if (status != NULL)
            *status = st;

        break;
    }

    case CMD_CASE: {
        char *word = expand_to_string(c->name);
        int   st = 0;

        for (int i = 0; i < c->narms; i++) {
            int hit = 0;

            for (int k = 0; c->arms[i].patterns[k] != NULL && !hit; k++) {
                char  *pat = expand_to_string(c->arms[i].patterns[k]);
                size_t plen = strlen(pat);

                /* A pattern written with quotes around parts of it is not a pattern at
                 * all, so the quotes are stripped and the whole thing is compared as a
                 * string.  `case $x in "a*")` matches `abc` literally, and that is the
                 * behaviour every shell has. */
                int quoted = strchr(c->arms[i].patterns[k], '"') != NULL ||
                             strchr(c->arms[i].patterns[k], '\'') != NULL;

                if (quoted) {
                    char *bare = malloc(plen + 1);

                    if (bare != NULL) {
                        size_t n = 0;

                        for (size_t j = 0; j < plen; j++) {
                            char ch = pat[j];

                            if (ch == '\\' && j + 1 < plen)
                                ch = pat[++j];

                            if (ch != '"' && ch != '\'')
                                bare[n++] = ch;
                        }

                        bare[n] = 0;
                        hit = strcmp(word, bare) == 0;
                        free(bare);
                    }
                } else {
                    hit = fnmatch_pattern(pat, word);
                }

                free(pat);
            }

            if (hit) {
                exec_cmd(c->arms[i].body, &st);

                if (c->arms[i].taken)
                    break;              /* `;;` -- do not fall through */

                /* `;&` falls through to the next arm's *body* without testing its pattern,
                 * which is what the POSIX fallthrough operator means. */
                continue;
            }
        }

        free(word);

        if (status != NULL)
            *status = st;

        break;
    }

    case CMD_FUNC:
        /* The body was taken when the function was defined; there is nothing to run. */
        if (status != NULL)
            *status = 0;

        break;

    default:
        if (status != NULL)
            *status = 0;

        break;
    }
}

/* --- running a string or a file --------------------------------------------------------- */

/* Run one line's worth of text, parsing and executing as it goes.
 *
 * Line by line, and that matters for `read`: a script that reads from its own standard
 * input has to see the next line *after* the current one has finished executing, or
 * `while read line; do ...; done < script` would consume the whole file in the first
 * `read`.  Parsing and running a line at a time is what makes the loop work. */
void exec_run_string(const char *text, int *status) {
    char *copy = strdup(text != NULL ? text : "");
    char *line = copy;
    int   last = 0;

    while (line != NULL && *line != 0 && !exiting) {
        char *nl = strchr(line, '\n');
        char *next = NULL;

        if (nl != NULL) {
            *nl = 0;
            next = nl + 1;
        }

        tokens_t toks;
        const char *err = NULL;

        if (lex_line(line, &toks) == 0) {
            cmd_t *c = parse_program(&toks, &err);

            if (c == NULL) {
                if (err != NULL)
                    sh_error("%s", err);
                else
                    last = 0;
            } else {
                /* An alias, expanded once at the head of the line.  Only in front of the
                 * first word, and only when it is a whole word: `alias ll='ls -l'` makes
                 * `ll /tmp` into `ls -l /tmp`, and must not make `call` into `ls -lall`. */
                if (c->kind == CMD_SIMPLE && c->nwords > 0 && c->words[0] != NULL) {
                    const char *a = alias_get(c->words[0]);

                    if (a != NULL && !in_background) {
                        char *rest = (char *)c->words[0] + strlen(c->words[0]);
                        char  *joined = malloc(strlen(a) + strlen(rest) + 1);

                        if (joined != NULL) {
                            strcpy(joined, a);
                            strcat(joined, rest);
                            free(c->words[0]);
                            c->words[0] = joined;
                        }
                    }
                }

                exec_cmd(c, &last);
                cmd_free(c);
            }
        }

        tokens_free(&toks);
        line = next;
    }

    free(copy);

    if (status != NULL)
        *status = last;
}

void exec_run_string_file(const char *path, int *status) {
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        sh_error("%s: %s", path, strerror(errno));

        if (status != NULL)
            *status = 127;

        return;
    }

    /* Read a line, expand nothing, hand it to the runner.  A `source`d file runs in this
     * process, so a `cd` in it moves the shell -- which is the entire reason `source` is a
     * builtin and not a program. */
    char   buf[8192];
    char  *leftover = NULL;
    int    last = 0;
    size_t n = 0;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && !exiting) {
        /* NUL-terminate, and stop at the first newline: what `fgets` would have done, and
         * reading a block rather than a line is what lets `read` inside the script see the
         * *next* line rather than the rest of the buffer. */
        buf[n] = 0;

        char *nl = memchr(buf, '\n', n);

        if (nl != NULL) {
            *nl = 0;

            /* Push the rest back, so that the next iteration of this loop sees it.  A
             * `FILE *` cannot be un-read, which is why the leftover is kept here. */
            if (nl + 1 < buf + n) {
                size_t rest = n - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rest);
                n = rest;
            } else {
                n = 0;
            }

            leftover = strdup(buf);
        } else {
            leftover = strdup(buf);
            n = 0;
        }

        int line_status = 0;

        exec_run_string(leftover != NULL ? leftover : "", &line_status);
        free(leftover);
        leftover = NULL;
        last = line_status;
    }

    /* A file with no final newline still has a last line, and running it is the difference
     * between a script that works and one that silently does nothing for its last command. */
    if (n > 0 && !exiting) {
        buf[n] = 0;
        exec_run_string(buf, &last);
    }

    fclose(f);

    if (status != NULL)
        *status = last;
}

/* --- the line reader -------------------------------------------------------------------- */

/* The prompt, and the line.
 *
 * Read with `read()` a character at a time rather than with `fgets()`, because the two
 * differ in what happens when a line arrives without a newline at the end of it: `fgets`
 * hands back a partial line and the shell would run half a command.  A character at a
 * time costs a syscall per character, which on a keyboard is nothing, and on a script's
 * standard input is a pipeline -- where the writer is blocked anyway. */
int sh_read_line(FILE *in, char **out) {
    size_t cap = 256;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (buf == NULL)
        return -1;

    for (;;) {
        int c = fgetc(in);

        if (c == EOF) {
            if (len == 0) {
                free(buf);
                return -1;
            }

            break;
        }

        if (c == '\n')
            break;

        if (len + 2 > cap) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL) {
                free(buf);
                return -1;
            }

            buf = grown;
            cap *= 2;
        }

        buf[len++] = (char)c;
    }

    buf[len] = 0;
    *out = buf;
    return 0;
}

/* --- what the shell is doing ------------------------------------------------------------ */

int sh_exiting(void) {
    return exiting;
}

int sh_exit_code(void) {
    return exit_code;
}

void sh_note_background(int on) {
    in_background = on;
}

int  sh_argc_of(char **v) {
    return args_n(v);
}

char **sh_env_of(void) {
    return var_environ();
}

/* `fnmatch` for the case statement, kept here so that exec.c's one use of it does not need
 * the include and so the "a pattern with quotes in it is not a pattern" rule has somewhere
 * to live. */
int fnmatch_pattern(const char *pattern, const char *text) {
    return fnmatch(pattern, text, 0) == 0;
}
