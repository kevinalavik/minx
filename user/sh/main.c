/* minx - `sh`: the program itself.
 *
 * Three modes and one loop:
 *
 *   `sh`                 an interactive shell on the terminal
 *   `sh -c 'text'`       run the text and exit with its status
 *   `sh script args...` run the file, with `args...` as `$1..`
 *
 * The interactive loop is the interesting one, and it does four things in an order that
 * each of them depends on:
 *
 *   1. **Set the terminal to character-at-a-time with no echo.**  A shell that reads a line
 *      a line at a time cannot use the kernel's line discipline -- the kernel would hand it
 *      whole lines and the kernel's own editing would fight the shell's history.  So the
 *      shell takes the terminal over: `-icanon` and `-echo`, and puts them back on the way
 *      out.  A shell that leaves the terminal in raw mode on the way out is a shell that
 *      leaves the user's terminal broken, which is why every exit path here restores it --
 *      including the one taken by `exit` from a script.
 *
 *   2. **Print a prompt**, but only when the input is a terminal.  A shell that prompts when
 *      its input is a pipe writes the prompt into the pipe, and every program that pipes
 *      something into it gets the prompt as the first line of its output.
 *
 *   3. **Read a line, a character at a time**, handling backspace and Ctrl-C and Ctrl-D
 *      itself, because with the kernel's discipline off there is nobody else to do it.
 *
 *   4. **Run it**, then set `$?`.
 *
 * The signal handling is the other thing worth stating.  `SIGINT` is *ignored* in the shell
 * and *default* in every child, so a ^C at an idle prompt does nothing and a ^C in the
 * middle of a running command stops that command and returns to the prompt.  Getting that
 * backwards is the difference between a shell you can interrupt and one you have to
 * reboot.
 */
#include "sh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* From jobs.c. */
void job_note_started(pid_t pid, const char *name);
void job_note_finished(pid_t pid, int status);
void job_before_wait(void);
void job_after_wait(void);

/* From var.c. */
void var_import_environ(char **envp);

/* --- history ------------------------------------------------------------------------ */

/* The history, kept as the lines themselves.
 *
 * Kept here rather than in the line reader because `history` has to print exactly what was
 * run, and a history built anywhere else is a history of something slightly different.  A
 * fixed table, for the reason the job table is fixed. */
#define HIST_MAX 64

static char  *history[HIST_MAX];
static int    history_at;
static int    history_count;

static void history_add(const char *line) {
    if (line[0] == 0)
        return;

    /* A line identical to the last one is not added twice.  Pressing up-arrow and hitting
     * return should not fill the history with the same command. */
    int newest = (history_at + HIST_MAX - 1) % HIST_MAX;

    if (history[newest] != NULL && strcmp(history[newest], line) == 0)
        return;

    free(history[history_at]);
    history[history_at] = strdup(line);
    history_at = (history_at + 1) % HIST_MAX;
    history_count = history_count < HIST_MAX ? history_count + 1 : HIST_MAX;

}

void history_print(void) {
    int n = history_count;

    for (int i = 0; i < n; i++) {
        int slot = (history_at - n + i + HIST_MAX * 2) % HIST_MAX;

        if (history[slot] != NULL)
            printf("%5d  %s\n", i + 1, history[slot]);
    }
}

/* --- the terminal ---------------------------------------------------------------------- */

/* The terminal's settings when the shell took it over, and the flag that says it did.
 *
 * The flag is what makes the restore safe to call from several places -- the normal exit,
 * `exit` from a script, and the `SIGINT` handler -- because restoring twice would restore
 * the *raw* settings the second time, which is how a terminal ends up invisible. */
static struct termios saved_termios;
static int            termios_saved;

static int raw_mode(void) {
    if (!isatty(0))
        return 0;

    if (tcgetattr(0, &saved_termios) != 0)
        return 0;

    struct termios raw = saved_termios;

    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);

    /* `VMIN = 0` with `VTIME = 1` makes `read()` return after a tenth of a second with
     * nothing, so the loop can notice a signal that arrived while it was waiting.  With
     * `VMIN = 1` a ^C would not be seen until a key was pressed. */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(0, TCSAFLUSH, &raw) != 0)
        return 0;

    termios_saved = 1;
    return 1;
}

static void restore_termios(void) {
    if (!termios_saved)
        return;

    tcsetattr(0, TCSAFLUSH, &saved_termios);
    termios_saved = 0;
}

/* --- the line reader --------------------------------------------------------------------- */

/* Which history slot the up-arrow is on, and -1 for "the line being typed". */
static int browsing = -1;
static char *pending = NULL;

static char *history_up(void) {
    if (history_count == 0)
        return NULL;

    if (browsing < 0) {
        free(pending);
        pending = NULL;
        browsing = 0;
    } else if (browsing + 1 < history_count) {
        browsing++;
    } else {
        return NULL;
    }

    int slot = (history_at - history_count + browsing + HIST_MAX * 2) % HIST_MAX;

    return history[slot];
}

static char *history_down(void) {
    if (browsing < 0)
        return NULL;

    if (browsing == 0) {
        /* Past the newest is the empty line, which is where a person goes when they have
         * changed their mind. */
        browsing = -1;
        return pending != NULL ? pending : NULL;
    }

    browsing--;

    int slot = (history_at - history_count + browsing + HIST_MAX * 2) % HIST_MAX;

    return history[slot];
}

/* Erase what is on the screen after the cursor and write `s` there. */
static void redraw(const char *s) {
    /* Back to the prompt's start, clear, rewrite.  Two escape sequences rather than a
     * line discipline feature, because the line discipline is off. */
    fputs("\r\033[K", stdout);
    fputs(sh.prompt, stdout);
    fputs(s, stdout);
    fflush(stdout);
}

/* One line, with the editing this shell has.
 *
 * Returns 0 for a line, -1 for end of input.  A line interrupted by ^C is discarded and the
 * read *continues*, because ^C at a prompt means "throw that away" and not "exit". */
static int read_line_interactive(char **out) {
    char  *buf = malloc(512);
    size_t len = 0;

    if (buf == NULL)
        return -1;

    buf[0] = 0;
    browsing = -1;
    free(pending);
    pending = NULL;

    fputs(sh.prompt, stdout);
    fflush(stdout);

    for (;;) {
        char c;

        if (read(0, &c, 1) != 1) {
            /* `VMIN`/`VTIME` gave us a timeout: either nothing has arrived, or a signal
             * did.  Loop and look again. */
            continue;
        }

        if (c == '\n') {
            putchar('\n');
            fflush(stdout);
            buf[len] = 0;
            *out = buf;
            return 0;
        }

        if (c == 4) {                      /* Ctrl-D */
            if (len == 0) {
                free(buf);
                return -1;
            }

            continue;
        }

        if (c == 3) {                      /* Ctrl-C */
            fputs("\n", stdout);
            free(buf);
            buf = malloc(512);
            len = 0;
            buf[0] = 0;
            browsing = -1;
            free(pending);
            pending = NULL;
            fputs(sh.prompt, stdout);
            fflush(stdout);
            continue;
        }

        if (c == 127 || c == 8) {          /* backspace */
            if (len > 0) {
                len--;
                buf[len] = 0;
                fputs("\b \b", stdout);
                fflush(stdout);
            }

            continue;
        }

        if (c == 21) {                     /* Ctrl-U: throw the line away */
            fputs("\r\033[K", stdout);
            fputs(sh.prompt, stdout);
            fflush(stdout);
            len = 0;
            buf[0] = 0;
            continue;
        }

        if (c == 12) {                     /* Ctrl-L: redraw */
            redraw(buf);
            continue;
        }

        if (c == 27) {                     /* an escape: the start of an arrow key */
            char seq[3];

            if (read(0, &seq[0], 1) != 1)
                continue;

            if (read(0, &seq[1], 1) != 1)
                continue;

            if (seq[0] == '[' && (seq[1] == 'A' || seq[1] == 'B')) {
                char *h = (seq[1] == 'A') ? history_up() : history_down();

                if (h != NULL) {
                    strcpy(buf, h);
                    len = strlen(buf);
                    redraw(buf);
                }

                continue;
            }

            continue;
        }

        if (c < 32)
            continue;                      /* any other control key: nothing */

        if (len + 2 >= 512) {
            char *grown = realloc(buf, len + 512);

            if (grown == NULL)
                break;

            buf = grown;
        }

        buf[len++] = c;
        buf[len] = 0;
        putchar(c);
        fflush(stdout);
    }

    buf[len] = 0;
    *out = buf;
    return 0;
}

/* --- signals ----------------------------------------------------------------------------- */

/* Nothing.
 *
 * ^C is ignored *here* and defaults in every child, and that asymmetry is the whole of an
 * interruptible shell.  The `SIGCHLD` handler does nothing on purpose: the shell reaps its
 * children where it waits for them, and a handler that also reaped them would take the exit
 * status away from `wait` and leave `jobs` unable to say "Done". */
static void on_signal(int sig) {
    (void)sig;
}

/* --- the main loop ------------------------------------------------------------------------- */

static int interactive;

/* `got_eof()`: did the last `read` stop at end of file?
 *
 * Tracked by a flag the `read` builtin sets, because the answer cannot be recovered from
 * the return value alone: `read` returning 1 means either "end of file" or "the line was
 * empty", and a `while read line` loop must stop on the first and not the second. */
static int last_read_eof;

int got_eof(int fd) {
    (void)fd;
    return last_read_eof;
}

void sh_set_read_eof(int yes) {
    last_read_eof = yes;
}

int signal_number(const char *name) {
    static const struct {
        const char *name;
        int         sig;
    } table[] = {
        { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT },
        { "KILL", SIGKILL }, { "TERM", SIGTERM }, { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
        { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
        { "TSTP", SIGTSTP },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0)
            return table[i].sig;

    return SIGTERM;
}

void job_before_wait(void) {
    sh_note_background(0);
}

void job_after_wait(void) {
    fflush(stdout);
}

int main(int argc, char **argv, char **envp) {
    int   status = 0;
    int   i = 1;
    char *command = NULL;
    char *script = NULL;

    sh.argv0 = argv[0] != NULL ? argv[0] : "sh";
    sh_name = sh.argv0;

    /* Options, in the order every shell accepts them: `sh -c text name args...` is the
     * form a program uses to run a command and is the reason `name` is the argument after
     * the text. */
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }

        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -c needs an argument\n", sh_name);
                return 2;
            }

            command = argv[i + 1];
            i += 2;
            break;
        }

        if (strcmp(argv[i], "-s") == 0) {
            script = NULL;               /* read the standard input */
            i++;
            continue;
        }

        /* Anything else is left for the script to see, which is what a shell does with
         * `sh -x script`: `-x` is not a shell option here, and swallowing it silently
         * would make `sh -x` mean something different from every other shell. */
        break;
    }

    /* Unbuffered from the first line: a shell that buffers its own output loses the last
     * thing it said when it dies, and an interactive shell that buffers the prompt does not
     * show the prompt at all until something else happens to flush it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* The environment.  Imported *unexported* -- see var.c -- and then PATH and HOME are
     * put back if they are missing, because `execvp` with no PATH searches nothing and a
     * shell whose first command fails is a shell that looks broken. */
    var_import_environ(envp);

    if (var_get("PATH") == NULL)
        var_set("PATH", "/bin:/usr/bin", 1);

    if (var_get("HOME") == NULL)
        var_set("HOME", "/", 1);

    {
        char cwd[1024];

        if (getcwd(cwd, sizeof(cwd)) != NULL)
            var_set("PWD", cwd, 1);
    }

    var_set("IFS", " \t\n", 0);
    var_set("PS1", interactive ? "$ " : "", 0);
    var_set("PS2", "> ", 0);
    var_set("OPTIND", "1", 0);
    var_set("OPTARG", "", 0);

    signal(SIGINT, on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGTERM, on_signal);

    /* `sh -c text`: one command, and the exit status is its status.  This is the form every
     * program uses, so it has to work before anything else does. */
    if (command != NULL) {
        /* `$0` is the name after the text, and `$1..` are what follows it -- which is what
         * makes `sh -c 'echo $0 $1' myname hello` print "myname hello". */
        sh.argv0 = (i < argc) ? argv[i] : "sh";

        for (int k = i + 1; k < argc; k++)
            ;

        sh.argc = argc - i - 1;
        sh.argv = malloc((size_t)(sh.argc + 1) * sizeof(char *));

        /* Copied, not pointed at.  `bi_set()` frees the old parameters before installing new
         * ones, so every set of parameters has to be one the shell owns -- and this one used
         * to be the C runtime's `argv`, which made the first `set --` in a script release
         * memory it never allocated.  A `sh -c 'set -- a b'` freed the string "sh -c" out
         * from under the loader, and nothing about that failure points at `set`. */
        if (sh.argv != NULL) {
            for (int k = 0; k < sh.argc; k++)
                sh.argv[k] = strdup(argv[i + 1 + k]);

            sh.argv[sh.argc] = NULL;
        }

        exec_run_string(command, &status);
        fflush(stdout);
        return sh_exiting() ? sh_exit_code() : status;
    }

    if (i < argc) {
        script = argv[i];
        i++;

        if (script[0] == '/' || strchr(script, '/') != NULL) {
            FILE *f = fopen(script, "r");

            if (f == NULL) {
                sh_error("%s: %s", script, strerror(errno));
                return 127;
            }

            fclose(f);
        } else {
            char **found = path_search(script);

            if (found == NULL) {
                sh_error("%s: not found", script);
                return 127;
            }

            free(found[0]);
            free(found);
        }

        sh.argc = argc - i;
        sh.argv = malloc((size_t)(sh.argc + 1) * sizeof(char *));

        /* Copied for the same reason as above: the script's own parameters are released by
         * `bi_set()` and by `bi_shift()`, and they come from the C runtime. */
        if (sh.argv != NULL) {
            for (int k = 0; k < sh.argc; k++)
                sh.argv[k] = strdup(argv[i + k]);

            sh.argv[sh.argc] = NULL;
        }

        exec_run_string_file(script, &status);
        fflush(stdout);
        return sh_exiting() ? sh_exit_code() : status;
    }

    /* Nothing to run: a shell.  Interactive only when the standard input is a terminal --
     * which is the rule that keeps the prompt out of a pipe. */
    interactive = isatty(0);

    if (interactive) {
        raw_mode();
        printf("%s\n", "minx sh -- type `help' if you have lost.");
    }

    sh.prompt = "$ ";

    for (;;) {
        char *line = NULL;

        if (interactive) {
            if (read_line_interactive(&line) != 0)
                break;
        } else {
            /* Not a terminal: no editing, no prompt, and the input is read a line at a
             * time so that a `read` inside the script sees the *next* line. */
            if (sh_read_line(stdin, &line) != 0)
                break;
        }

        history_add(line);
        exec_run_string(line, &status);
        free(line);

        if (sh_exiting())
            break;
    }

    restore_termios();
    fflush(stdout);

    return sh_exiting() ? sh_exit_code() : status;
}
