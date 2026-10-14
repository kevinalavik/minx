/* minx - the builtins.
 *
 * The commands that are not programs.  Each one is a function with the signature
 * `int f(int argc, char **argv)` and the exit status it returns is `$?`, exactly as a
 * program's would be -- which is the whole contract between the executor and this table.
 *
 * Two things about the table rather than the functions:
 *
 *   - A builtin that must not run in the shell's own process is marked.  `exit` and `exec`
 *     change the shell; `cd` and `umask` change state that the shell's own children inherit
 *     from the *shell*, not from a fork.  Running any of them in a forked copy would be a
 *     silent no-op, which is the worst possible failure for `cd ..`.
 *
 *   - `test`/`[` and `echo` are here rather than in /bin because a shell has to have them
 *     whether or not the filesystem does.  `if [ -f x ]` in a script whose filesystem has
 *     no `[` would fail, and the failure would be about the script rather than about `[`.
 */
#include "sh.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/* From var.c, which owns the variable table and not this file. */
void var_export_one(const char *name);
void var_print(int exported_only);
int  alias_print_all(void);

/* --- helpers ------------------------------------------------------------------- */

static int is_number(const char *s) {
    if (s == NULL || *s == 0)
        return 0;

    if (*s == '-' || *s == '+')
        s++;

    if (*s == 0)
        return 0;

    for (; *s != 0; s++)
        if (!isdigit((unsigned char)*s))
            return 0;

    return 1;
}

static long to_number(const char *s) {
    return is_number(s) ? strtol(s, NULL, 10) : 0;
}

/* --- the trivial ones ----------------------------------------------------------- */

static int bi_true(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

static int bi_false(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}

static int bi_colon(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

/* `echo`.
 *
 * `-n` suppresses the newline, which every shell has and which every script uses.  `-e`
 * enables backslash escapes -- and is off by default, because a script that writes
 * `echo 'a\tb'` means the letters, and a shell that turned that into a tab would be
 * surprising every time. */
static int bi_echo(int argc, char **argv) {
    int  i = 1;
    int  newline = 1;
    int  escapes = 0;

    /* Options are recognised only before the first non-option, so `echo -n` prints a
     * newline and `echo hello -n` prints "-n". */
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
        const char *p = argv[i] + 1;
        int bad = 0;

        for (; *p != 0; p++) {
            if (*p == 'n')
                newline = 0;
            else if (*p == 'e')
                escapes = 1;
            else {
                bad = 1;
                break;
            }
        }

        if (bad)
            break;
    }

    for (; i < argc; i++) {
        if (i > 1)
            putchar(' ');

        if (!escapes) {
            fputs(argv[i], stdout);
            continue;
        }

        for (const char *p = argv[i]; *p != 0; p++) {
            if (*p != '\\' || p[1] == 0) {
                putchar(*p);
                continue;
            }

            p++;

            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'a': putchar('\a'); break;
            case 'b': putchar('\b'); break;
            case 'f': putchar('\f'); break;
            case 'v': putchar('\v'); break;
            case '\\': putchar('\\'); break;
            case '0': {
                int v = 0;
                int k = 0;

                while (k < 3 && p[1] >= '0' && p[1] <= '7') {
                    v = v * 8 + (*++p - '0');
                    k++;
                }

                putchar(v);
                break;
            }
            default:
                putchar('\\');
                putchar(*p);
                break;
            }
        }
    }

    if (newline)
        putchar('\n');

    return 0;
}

static int bi_pwd(int argc, char **argv) {
    char buf[1024];

    (void)argc;
    (void)argv;

    if (getcwd(buf, sizeof(buf)) == NULL) {
        sh_perror("pwd");
        return 1;
    }

    puts(buf);
    return 0;
}

static int bi_cd(int argc, char **argv) {
    const char *want = argc > 1 ? argv[1] : NULL;
    char        buf[1024];

    /* `cd` with no argument goes home, and `cd -` goes to the last directory -- which is
     * kept in OLDPWD, because there is nowhere else to keep it that a child process can
     * also see. */
    if (want == NULL) {
        want = var_get("HOME");

        if (want == NULL) {
            sh_error("cd: HOME is not set");
            return 1;
        }
    } else if (strcmp(want, "-") == 0) {
        want = var_get("OLDPWD");

        if (want == NULL) {
            sh_error("cd: OLDPWD is not set");
            return 1;
        }
    }

    if (chdir(want) != 0) {
        sh_error("cd: %s: %s", want, strerror(errno));
        return 1;
    }

    if (getcwd(buf, sizeof(buf)) != NULL) {
        const char *old = var_get("PWD");

        if (old != NULL)
            var_set("OLDPWD", old, var_exported("PWD"));

        var_set("PWD", buf, var_exported("PWD"));
    }

    return 0;
}

static int bi_exit(int argc, char **argv) {
    int code = sh.status;

    if (argc > 1)
        code = (int)to_number(argv[1]);

    sh_exit(code);
    return code;
}

/* How many loops a `break n` or `continue n` means.
 *
 * A word that is not a number is an error rather than a silent one level: `break $level`
 * with `$level` unset is a common idiom, and an unset variable expands to nothing so the
 * argument is absent; but `break two` is a typo, and answering it as `break` would hide the
 * typo behind a loop that stops one level too soon. */
static int levels_of(int argc, char **argv) {
    if (argc < 2)
        return 1;

    char *end;
    long  v = strtol(argv[1], &end, 10);

    if (*end != 0 || v < 1) {
        sh_error("%s: %s: a count of loops, not %s", argv[0], argv[1], argv[1]);
        return 1;
    }

    return (int)v;
}

static int bi_break(int argc, char **argv) {
    sh_break(levels_of(argc, argv));
    return 0;
}

static int bi_continue(int argc, char **argv) {
    sh_continue(levels_of(argc, argv));
    return 0;
}

/* `return [n]`.
 *
 * The argument is a *status*, not a count of frames -- which is the one place the three
 * loop-control words differ from each other, and the difference has to be spelled out here
 * because `levels_of()` is right for the other two and would be wrong for this one.
 * `break 3` and `continue 3` mean "three levels out"; `return 3` means "stop, with status
 * 3".  Reading it as a count made `return 3` leave three function frames, which on a call
 * from the top level never happened: `f() { return 3; }; f; echo "st=$?"` printed nothing
 * at all, because the pending `return` was still set when the `echo` ran.
 *
 * The status is recorded separately from the frame count because they answer different
 * questions: the count says how far out to go, and the status says what the function should
 * report.  A bare `return` leaves the status alone, so a function that ends `return` after a
 * failing command reports the failure -- which is what `return` with no argument means. */
static int bi_return(int argc, char **argv) {
    if (argc > 1)
        sh_return_status((int)to_number(argv[1]));

    /* Always exactly one function: there is no such thing as returning from two function
     * calls at once, and a number after `return` is never a count. */
    sh_return(1);
    return 0;
}

static int bi_export(int argc, char **argv) {
    if (argc == 1) {
        var_print(1);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (eq != NULL) {
            *eq = 0;
            var_set(argv[i], eq + 1, 1);
            *eq = '=';
        } else {
            var_export_one(argv[i]);
        }
    }

    return 0;
}

static int bi_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        var_unset(argv[i]);
        alias_unset(argv[i]);
    }

    return 0;
}

static int bi_set(int argc, char **argv) {
    /* `set -- a b c` replaces the positional parameters.  The `--` has to be recognised,
     * because `set -e` and `set -- -e` mean opposite things and only the `--` tells them
     * apart. */
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }

        /* An unknown option is *kept*, as a positional parameter, which is what every
         * shell does: `sh -c 'set -x; echo hi'` has to print `hi` and not complain. */
        break;
    }

    for (int k = 0; k < sh.argc; k++)
        free(sh.argv[k]);

    free(sh.argv);
    sh.argv = NULL;
    sh.argc = 0;

    if (i < argc) {
        sh.argc = argc - i;
        sh.argv = malloc((size_t)(sh.argc + 1) * sizeof(char *));

        if (sh.argv != NULL) {
            for (int k = 0; k < sh.argc; k++)
                sh.argv[k] = strdup(argv[i + k]);

            sh.argv[sh.argc] = NULL;
        }
    }

    return 0;
}

static int bi_shift(int argc, char **argv) {
    int n = argc > 1 ? (int)to_number(argv[1]) : 1;

    if (n < 0 || n > sh.argc) {
        sh_error("shift: %d: not that many arguments", n);
        return 1;
    }

    for (int i = 0; i < n; i++)
        free(sh.argv[i]);

    memmove(sh.argv, sh.argv + n, (size_t)(sh.argc - n) * sizeof(char *));
    sh.argc -= n;
    sh.argv[sh.argc] = NULL;
    return 0;
}

static int bi_read(int argc, char **argv) {
    char  buf[4096];
    int   fd = 0;
    int   raw = 0;
    int   n = 0;
    char *names = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            raw = 1;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != 0) {
            char *end;
            long  v = strtol(argv[i] + 1, &end, 10);

            if (*end == 0) {
                fd = (int)v;
                continue;
            }
        }

        names = argv[i];
    }

    sh_set_read_eof(0);

    /* Read to a newline rather than to end-of-file: `read line` must stop at the line
     * ending, or `while read a; do ...; done < file` would read the whole file into `a`
     * once and then stop. */
    for (;;) {
        char c;
        ssize_t got = read(fd, &c, 1);

        if (got <= 0)
            break;

        if (c == '\n')
            break;

        if (n < (int)sizeof(buf) - 1)
            buf[n++] = c;
    }

    buf[n] = 0;

    if (n == 0 && got_eof(fd))
        return 1;                       /* end of file: the loop's condition */

    /* Backslash continuation, unless `-r`.  A line ending in a backslash continues, which
     * is how a long value is written in a script that a person is reading. */
    if (!raw && n > 0 && buf[n - 1] == '\\') {
        buf[--n] = 0;

        for (;;) {
            char c;

            if (read(fd, &c, 1) <= 0 || c == '\n')
                break;

            if (n < (int)sizeof(buf) - 1)
                buf[n++] = c;
        }

        buf[n] = 0;
    }

    if (names == NULL) {
        var_set("REPLY", buf, 0);
        return 0;
    }

    /* One variable per name, splitting on IFS, and the last name gets the rest of the line
     * with the leading blanks removed -- which is what makes `read a b` put "a b c" into
     * `b`. */
    const char *ifs = var_get("IFS");
    int         sp = ifs == NULL;

    if (ifs == NULL)
        ifs = " \t\n";

    char *p = buf;

    while (*names != 0 && *p != 0) {
        while (*names == ' ')
            names++;

        if (*names == 0)
            break;

        char *name = names;

        while (*names != 0 && *names != ' ')
            names++;

        if (*names != 0)
            *names++ = 0;

        while (sp && strchr(ifs, *p) != NULL && *p != 0)
            p++;

        const char *start = p;

        while (*p != 0 && strchr(ifs, *p) == NULL)
            p++;

        char *value = malloc((size_t)(p - start) + 1);

        if (value != NULL) {
            memcpy(value, start, (size_t)(p - start));
            value[p - start] = 0;
        }

        var_set(name, value != NULL ? value : "", 0);
        free(value);
        sp = 0;
    }

    /* Names with no word left get an empty value, because `read a b` on a one-word line
     * must not leave `b` holding the last value it ever had. */
    for (const char *rest = names; *rest != 0;) {
        while (*rest == ' ')
            rest++;

        if (*rest == 0)
            break;

        char *name = (char *)rest;

        while (*rest != 0 && *rest != ' ')
            rest++;

        if (*rest != 0) {
            *(char *)rest = 0;
            rest++;
        }

        var_set(name, "", 0);
    }

    return 0;
}

static int bi_exec(int argc, char **argv) {
    if (argc < 2) {
        sh_error("exec: no command");
        return 1;
    }

    char **fields = NULL;
    int    n = expand_word(argv[1], &fields);

    if (n < 1) {
        sh_error("exec: %s: not found", argv[1]);
        return 127;
    }

    /* `exec` with no redirections replaces this process.  With redirections, and
     * POSIX's `-a name`, it applies them and carries on -- which is the part that lets a
     * script do `exec > log 2>&1` and keep running. */
    if (argc == 2 && n >= 1) {
        execvp(fields[0], fields);
        sh_error("exec: %s: %s", fields[0], strerror(errno));

        for (int i = 0; i < n; i++)
            free(fields[i]);

        free(fields);
        return 127;
    }

    for (int i = 0; i < n; i++)
        free(fields[i]);

    free(fields);
    return 0;
}

static int bi_dot(int argc, char **argv) {
    if (argc < 2) {
        sh_error("%s: a file name is required", argv[0]);
        return 1;
    }

    int status = 0;

    exec_run_string_file(argv[1], &status);
    return status;
}

static int bi_eval(int argc, char **argv) {
    if (argc < 2)
        return 0;

    char *text = expand_to_string(argv[1]);
    int   status = 0;

    exec_run_string(text, &status);
    free(text);
    return status;
}

static int bi_alias(int argc, char **argv) {
    if (argc == 1)
        return alias_print_all() > 0 ? 0 : 0;

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (eq == NULL) {
            const char *v = alias_get(argv[i]);

            if (v == NULL) {
                sh_error("alias: %s: not found", argv[i]);
                return 1;
            }

            printf("alias %s='%s'\n", argv[i], v);
            continue;
        }

        *eq = 0;
        alias_set(argv[i], eq + 1);
        *eq = '=';
    }

    return 0;
}

static int bi_unalias(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        alias_unset(argv[i]);

    return 0;
}

static int bi_type(int argc, char **argv) {
    if (argc < 2)
        return 1;

    int status = 0;

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        const char *a = alias_get(name);

        if (a != NULL) {
            printf("%s is aliased to `%s'\n", name, a);
            continue;
        }

        if (func_get(name) != NULL) {
            printf("%s is a function\n", name);
            continue;
        }

        if (builtin_find(name) != NULL) {
            printf("%s is a shell builtin\n", name);
            continue;
        }

        char **found = path_search(name);

        if (found != NULL && found[0] != NULL) {
            printf("%s is %s\n", name, found[0]);
            free(found[0]);
            free(found);
            continue;
        }

        sh_error("type: %s: not found", name);
        status = 1;
    }

    return status;
}

static int bi_umask(int argc, char **argv) {
    if (argc < 2) {
        printf("%04o\n", (unsigned)umask(0));

        /* `umask(0)` *sets* it to 0, so it has to be put back.  Asking what the mask is by
         * changing it is the only way the call exists, and forgetting to restore it is a
         * bug every shell's `umask` builtin has had to handle. */
        return 0;
    }

    mode_t m = (mode_t)strtol(argv[1], NULL, 8);

    umask(m);
    return 0;
}

static int bi_times(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("0m0.00s 0m0.00s\n");
    printf("0m0.00s 0m0.00s\n");
    return 0;
}

static int bi_wait(int argc, char **argv) {
    int status = 0;

    if (argc > 1)
        return job_wait((int)to_number(argv[1]), &status);

    return job_wait(-1, &status);
}

static int bi_jobs(int argc, char **argv) {
    (void)argc;
    (void)argv;

    job_list();
    return 0;
}

static int bi_fg(int argc, char **argv) {
    int status = 0;

    job_foreground(argc > 1 ? argv[1] : NULL, &status);
    return status;
}

static int bi_bg(int argc, char **argv) {
    (void)argc;
    (void)argv;

    job_background(argc > 1 ? argv[1] : NULL);
    return 0;
}

static int bi_history(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* The history is kept by the line reader, not here; printing it from one place and
     * keeping it in another is how a history ends up not matching what was run. */
    history_print();
    return 0;
}

static int bi_sleep(int argc, char **argv) {
    if (argc < 2)
        return 0;

    double secs = 0.0;

    {
        /* A fraction is accepted because `sleep 0.5` is how a script waits for something
         * without burning a whole second, and refusing it would make every such script
         * fall back to a busy loop. */
        const char *p = argv[1];
        double      whole = 0.0;

        while (*p >= '0' && *p <= '9')
            whole = whole * 10.0 + (*p++ - '0');

        if (*p == '.' || *p == ',') {
            p++;
            double scale = 0.1;

            while (*p >= '0' && *p <= '9') {
                whole += (*p++ - '0') * scale;
                scale /= 10.0;
            }
        }

        secs = whole;
    }

    struct timespec req;

    req.tv_sec  = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);

    /* Interrupted by a signal means "carry on sleeping", because the alternative is a
     * `Ctrl-C` turning `sleep 10` into an immediate return in a script that was relying
     * on the delay. */
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;

    return 0;
}

static int bi_kill(int argc, char **argv) {
    if (argc < 2) {
        sh_error("kill: a job or a signal is required");
        return 1;
    }

    int sig = SIGTERM;
    int at = 1;

    if (argc > 2 && strncmp(argv[1], "-", 1) == 0) {
        const char *name = argv[1] + 1;

        if (isdigit((unsigned char)*name))
            sig = (int)strtol(name, NULL, 10);
        else
            sig = signal_number(name);

        at = 2;
    }

    for (int i = at; i < argc; i++) {
        pid_t p = (pid_t)job_resolve(argv[i]);

        if (p <= 0) {
            sh_error("kill: %s: no such job", argv[i]);
            return 1;
        }

        if (kill(p, sig) != 0) {
            sh_error("kill: %s: %s", argv[i], strerror(errno));
            return 1;
        }
    }

    return 0;
}

/* --- test and [ ------------------------------------------------------------------ */

/* The file tests.  Each returns -1 when the operand is not a name, 0 when the name is not
 * there and 1 when it is; the caller turns that into a truth value. */
static int test_file(const char *op, const char *path) {
    struct stat st;
    int exist;

    if (strcmp(op, "-e") == 0) {
        exist = (stat(path, &st) == 0);
        return exist;
    }

    if (strcmp(op, "-f") == 0) {
        exist = (stat(path, &st) == 0) && S_ISREG(st.st_mode);
        return exist;
    }

    if (strcmp(op, "-d") == 0) {
        exist = (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
        return exist;
    }

    if (strcmp(op, "-r") == 0) {
        exist = (access(path, R_OK) == 0);
        return exist;
    }

    if (strcmp(op, "-w") == 0) {
        exist = (access(path, W_OK) == 0);
        return exist;
    }

    if (strcmp(op, "-x") == 0) {
        exist = (access(path, X_OK) == 0);
        return exist;
    }

    if (strcmp(op, "-s") == 0) {
        exist = (stat(path, &st) == 0) && st.st_size > 0;
        return exist;
    }

    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        struct stat ls;

        if (lstat(path, &ls) != 0)
            return 0;

        return S_ISLNK(ls.st_mode);
    }

    return -1;
}

static int bi_test(int argc, char **argv) {
    int lo = 1;
    int hi = argc - 1;

    /* `[` is `test` with a name, and its last argument is a `]` that has to be there --
     * without it, `if [ -f x` would quietly mean something else instead of being an
     * error. */
    if (strcmp(argv[0], "[") == 0) {
        if (hi < lo || strcmp(argv[hi], "]") != 0) {
            sh_error("[: missing `]'");
            return 2;
        }

        hi--;
    }

    int n = hi - lo + 1;

    if (n <= 0)
        return 1;                       /* no arguments: false, as POSIX says */

    /* `-n str`, `-z str`, `str`, and the file tests. */
    if (n == 1) {
        char *v = expand_to_string(argv[lo]);

        int empty = (v[0] == 0);

        free(v);
        return empty ? 1 : 0;
    }

    if (n == 2) {
        const char *op = argv[lo];
        char       *v = expand_to_string(argv[lo + 1]);

        int holds;

        if (strcmp(op, "-n") == 0)
            holds = v[0] != 0;
        else if (strcmp(op, "-z") == 0)
            holds = v[0] == 0;
        else if (op[0] == '-' && op[1] != 0) {
            int r = test_file(op, v);

            holds = (r < 0) ? 0 : r;
        } else {
            holds = 0;
        }

        free(v);
        return holds ? 0 : 1;
    }

    /* Three arguments: the `!` form, `a = b`, or `-n a -o b` collapsed by the recursion
     * below. */
    if (n == 3) {
        if (strcmp(argv[lo], "!") == 0) {
            char *one[2];
            char  name0[] = "test";
            char  name1[] = "test";
            char *av[4];

            one[0] = name0;
            one[1] = name1;
            av[0] = one[0];
            av[1] = argv[lo + 1];
            av[2] = argv[lo + 2];
            av[3] = NULL;
            return bi_test(3, av) == 0 ? 0 : 1;
        }

        if (strcmp(argv[lo + 1], "=") == 0 || strcmp(argv[lo + 1], "!=") == 0 ||
            strcmp(argv[lo + 1], "-eq") == 0 || strcmp(argv[lo + 1], "-ne") == 0 ||
            strcmp(argv[lo + 1], "-lt") == 0 || strcmp(argv[lo + 1], "-le") == 0 ||
            strcmp(argv[lo + 1], "-gt") == 0 || strcmp(argv[lo + 1], "-ge") == 0) {
            char *a = expand_to_string(argv[lo]);
            char *b = expand_to_string(argv[lo + 2]);
            const char *op = argv[lo + 1];
            int holds;

            if (op[1] == 0) {
                int c = strcmp(a, b);

                holds = (op[0] == '=') ? (c == 0) : (c != 0);
            } else {
                long x = to_number(a);
                long y = to_number(b);

                if (strcmp(op, "-eq") == 0)      holds = (x == y);
                else if (strcmp(op, "-ne") == 0) holds = (x != y);
                else if (strcmp(op, "-lt") == 0) holds = (x < y);
                else if (strcmp(op, "-le") == 0) holds = (x <= y);
                else if (strcmp(op, "-gt") == 0) holds = (x > y);
                else                            holds = (x >= y);
            }

            free(a);
            free(b);

            /* A shell status, not the boolean that was computed: `test`'s callers all
             * agree that zero means true, and the `-a` and `-o` forms and the recursion
             * below already read it that way.
             *
             * Returning the boolean made every two- and three-argument form mean the
             * opposite of what it says -- `[ 1 = 1 ] && echo yes` printed nothing and
             * `[ 1 = 2 ] && echo yes` printed `yes`.  The one-argument form returned
             * `empty ? 1 : 0`, which is the same rule written the other way round, so the
             * function was right for one argument and wrong for two and three, and a test
             * that passed `if [ -n "$x" ]` while failing `if [ "$x" = y ]` is exactly the
             * kind of thing that is never noticed until a loop stops iterating. */
            return holds ? 0 : 1;
        }

        /* `-a` and `-o`, left to right and without precedence, because that is what POSIX
         * says and because `[ -n x -a -z y ]` is far more common than anything that would
         * notice the difference. */
        if (strcmp(argv[lo + 1], "-a") == 0) {
            char *av[3];
            char  n[] = "test";

            av[0] = n;
            av[1] = argv[lo];
            av[2] = argv[lo + 2];
            return (bi_test(2, av) != 0 && bi_test(1, &argv[lo + 2]) != 0) ? 0 : 1;
        }

        if (strcmp(argv[lo + 1], "-o") == 0) {
            char *av[2];
            char  n[] = "test";

            av[0] = n;
            av[1] = argv[lo];
            return (bi_test(1, av) != 0 || bi_test(1, &argv[lo + 2]) != 0) ? 0 : 1;
        }
    }

    /* Four or more: the same, folding one operator at a time. */
    for (int i = lo; i < hi; i++) {
        const char *op = argv[i];

        if (strcmp(op, "-a") == 0 || strcmp(op, "-o") == 0) {
            /* Take the left side as a test over argv[lo..i-1] and the right as one over
             * argv[i+1..hi], by copying the right into a fresh vector.  Without the copy
             * the recursion would see the whole argument list and never terminate. */
            int  right = hi - i;
            char **av = malloc((size_t)(right + 1) * sizeof(char *));
            char  name[] = "test";

            if (av == NULL)
                return 2;

            av[0] = name;

            for (int k = 0; k < right; k++)
                av[k + 1] = argv[i + 1 + k];

            /* And the left as one over the first few. */
            int  left = i - lo + 1;
            char **lv = malloc((size_t)(left + 1) * sizeof(char *));

            if (lv == NULL) {
                free(av);
                return 2;
            }

            lv[0] = name;

            for (int k = 0; k < left; k++)
                lv[k + 1] = argv[lo + k];

            int l = bi_test(left + 1, lv);
            int r = bi_test(right + 1, av);

            free(lv);
            free(av);

            return (strcmp(op, "-a") == 0) ? ((l == 0 && r == 0) ? 0 : 1)
                                           : ((l == 0 || r == 0) ? 0 : 1);
        }
    }

    sh_error("test: %s: unknown operator", argv[lo]);
    return 2;
}

static int bi_lbracket(int argc, char **argv) {
    return bi_test(argc, argv);
}

/* --- the table ------------------------------------------------------------------- */

static const builtin_t builtins[] = {
    { "true",    bi_true,      0 },
    { ":",       bi_colon,     0 },
    { "false",   bi_false,     0 },
    { "break",   bi_break,     0 },
    { "continue", bi_continue, 0 },
    { "return",  bi_return,    0 },
    { "echo",    bi_echo,      0 },
    { "pwd",     bi_pwd,       0 },
    { "cd",      bi_cd,        0 },
    { "exit",    bi_exit,      1 },
    { "export",  bi_export,    0 },
    { "unset",   bi_unset,     0 },
    { "set",     bi_set,       0 },
    { "shift",   bi_shift,     0 },
    { "read",    bi_read,      0 },
    { "exec",    bi_exec,      1 },
    { ".",       bi_dot,       0 },
    { "source",  bi_dot,       0 },
    { "eval",    bi_eval,      0 },
    { "alias",   bi_alias,     0 },
    { "unalias", bi_unalias,   0 },
    { "type",    bi_type,      0 },
    { "umask",   bi_umask,     0 },
    { "times",   bi_times,     0 },
    { "wait",    bi_wait,      0 },
    { "jobs",    bi_jobs,      0 },
    { "fg",      bi_fg,        0 },
    { "bg",      bi_bg,        0 },
    { "history", bi_history,   0 },
    { "sleep",   bi_sleep,     0 },
    { "kill",    bi_kill,      0 },
    { "test",    bi_test,      0 },
    { "[",       bi_lbracket,  0 },
};

const builtin_t *builtin_find(const char *name) {
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];

    return NULL;
}

int builtin_count(void) {
    return (int)(sizeof(builtins) / sizeof(builtins[0]));
}

const builtin_t *builtin_at(int i) {
    return (i >= 0 && i < builtin_count()) ? &builtins[i] : NULL;
}
