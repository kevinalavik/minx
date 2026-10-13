/* minx - `/sbin/init`: PID 1.
 *
 * The first program a user program ever meets, and the only one whose death is a system
 * failure.  Everything else on the machine can be restarted; this cannot, which is why the
 * jobs below are the shortest ones in the system and why the loop is the whole program.
 *
 * What it is responsible for, in the order it does them:
 *
 *   1. `/proc` and `/dev`.  The kernel has already mounted both, because a machine with no
 *      way to ask what it is cannot be reported on.  The calls here are the ones an init
 *      makes, and a kernel that got there first turns them into no-ops -- so the code that
 *      works on a kernel that has not is still here and still correct.
 *
 *   2. The console.  `/dev/console` is opened and put on 0, 1 and 2, after which this
 *      program never refers to "the console" again.  From that point every message is
 *      written to a descriptor, which is what makes the text the same whether it ends up
 *      on the framebuffer, in the serial mirror, or in a pipe a test put there.
 *
 *   3. A session, so that the console can be a controlling terminal.  Without it a `^C` at
 *      the shell has no session to interrupt and the terminal's line discipline has nobody
 *      to deliver it to.
 *
 *   4. `/bin/sh`, as a child.  Forked rather than exec'd, because init has to outlive the
 *      shell: it is what reaps orphans, what restarts the shell if it dies, and what is
 *      asked to stop the machine.
 *
 *   5. Orphans.  Every child it does not recognise as the shell is reaped silently, which
 *      is the whole of "init reaps orphans".  The kernel moves a dead task's children to
 *      PID 1; all that is left for this program is to call `wait` and throw the answer
 *      away, and a process that exits with nobody to wait for it never has its address
 *      space freed.
 *
 *   6. The end.  When the shell exits for the last time, init stops the machine, because
 *      the alternative is a black screen with a process list on it.  `RB_AUTOBOOT` is the
 *      value that reboots; every other value powers off, and that is a deliberate choice:
 *      a machine that has nothing left to run should stop, and a machine that should start
 *      again has something left to run.
 *
 * Two signals are ignored, and one is the reason this paragraph exists: PID 1 must not die
 * of a `^C`.  A user who types `^C` at a shell that is not reading, or a terminal that
 * sends SIGINT to the whole foreground group including init, would otherwise take the
 * machine's process tree down with it.  Every Unix does this and every Unix gets it for
 * the same reason.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

/* Where the shell is.  A variable rather than a literal because a system with no shell
 * should be able to say "run this instead" without a rebuild -- which is the whole reason
 * `init` is a program rather than a branch in the kernel. */
#define SHELL_PATH  "/bin/sh"
#define SHELL_PROG  "sh"

/* How many times the shell may exit in a row before init concludes that a shell which
 * cannot stay alive is worse than no shell.
 *
 * A crash loop is the failure mode this prevents, and it is a real one: a shell that
 * cannot find its libraries exits immediately, and an init that restarts it without
 * counting turns a broken system into a machine that fills its log with the same line and
 * never becomes usable.  Five is generous -- a user who typed `exit` five times in a row
 * is not a mistake, and neither is a shell being restarted on purpose -- and the message
 * says what happened rather than the machine just stopping.
 */
#define SHELL_RESPAWN_LIMIT 5

static pid_t shell_pid = -1;

/* How the shell ended, and whether it was the end of the world.
 *
 * Returns non-zero when init should stop the machine rather than start another shell.  The
 * two cases it treats as final are the two a person would recognise: the shell exiting
 * cleanly, which is what `exit` at a prompt does, and being killed by SIGKILL, which is
 * what a kernel panic or an OOM killer does and which will happen again if init restarts
 * into it. */
static int shell_exit_is_final(int status) {
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
        return 1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;

    return 0;
}

static void say(const char *s) {
    /* Through descriptor 1 and not through `stdout`, because `stdout` is a buffered stream
     * and the last thing this program will ever do is stop the machine: a message sitting
     * in a buffer when the machine goes off is a message nobody reads. */
    ssize_t ignored = write(1, s, strlen(s));

    (void)ignored;
}

static void say_num(long v) {
    char buf[24];
    int  n = 0;

    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }

    if (v == 0)
        buf[n++] = '0';

    while (v > 0 && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    for (int i = n - 1; i >= 0; i--) {
        ssize_t ignored = write(1, &buf[i], 1);

        (void)ignored;
    }
}

/* --- the child ------------------------------------------------------------------- */

/* Become the shell.
 *
 * Reached with `fork()` returning 0 and never returns: the `exec` either succeeds, in which
 * case this process *is* the shell and the code below is gone, or it fails, in which case
 * the only useful thing to do is say so and exit non-zero.  Returning would leave a second
 * init running, which is the one thing PID 1 must never be. */
static void become_shell(void) {
    char *argv[2];

    argv[0] = (char *)SHELL_PROG;
    argv[1] = NULL;

    /* The console on all three, unconditionally.  They may already be this; `dup2` to the
     * same descriptor is defined to do nothing, which is the case that matters because a
     * shell started by hand with `sh < file` has them already. */
    dup2(0, 0);
    dup2(1, 1);
    dup2(2, 2);

    execv(SHELL_PATH, argv);

    say("init: exec ");
    say(SHELL_PATH);
    say(" failed: ");
    say(strerror(errno));
    say("\n");

    /* 127 is the shell's own "command not found", and reusing it means a caller that
     * checks `$?` cannot tell this from a shell that could not find the command either --
     * which is the same class of problem from the user's side. */
    _exit(127);
}

/* --- the two filesystems --------------------------------------------------------- */

static void mount_one(const char *what, const char *where, const char *type) {
    if (mount(type, where, type, 0, NULL) == 0)
        return;

    /* Already mounted is the expected case, not a problem to report: the kernel mounts
     * both before it runs this program, and a message every boot saying so would train
     * everyone to ignore init's output. */
    if (errno == EBUSY)
        return;

    say("init: could not mount ");
    say(what);
    say(" on ");
    say(where);
    say(": ");
    say(strerror(errno));
    say("\n");
}

/* --- the console ----------------------------------------------------------------- */

static int open_console(void) {
    int fd = open("/dev/console", O_RDWR);

    if (fd < 0) {
        /* Not fatal, and deliberately not: on a kernel that has no devfs the descriptors
         * the loader opened *are* the console, and the machine is perfectly usable.  Saying
         * so and carrying on is better than refusing to boot over a convenience. */
        say("init: no /dev/console (");
        say(strerror(errno));
        say("); using the descriptors init was started with\n");
        return -1;
    }

    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);

    if (fd > 2)
        close(fd);

    return fd;
}

/* --- main ------------------------------------------------------------------------ */

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    say("\n[sbin/init] starting, pid ");
    say_num((long)getpid());
    say("\n");

    /* --- signals, before anything can arrive ---------------------------------- */

    /* Ignored, and not merely defaulted, because "ignore" and "the default" are different
     * answers for SIGCHLD on some systems and because a default of SIG_DFL for SIGINT is a
     * terminal interrupt and this process must not honour one. */
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);

    /* SIGCHLD is *not* ignored and SIGPIPE is not either.  A shell needs to be told that a
     * background job finished -- that is what `wait` and job control are -- and a program
     * that writes to a pipe nobody is reading must get EPIPE rather than dying quietly. */

    /* --- filesystems ------------------------------------------------------------ */

    mount_one("/dev",  "/dev",  "devfs");
    mount_one("/proc", "/proc", "procfs");

    /* --- the console ------------------------------------------------------------ */

    if (open_console() >= 0)
        say("[sbin/init] console is on 0, 1 and 2\n");

    /* --- the session ------------------------------------------------------------ */

    /* A session of its own, so the console can be a controlling terminal and so that a
     * `^C` goes to the foreground *group* rather than to every process on the machine.
     * Failing with EPERM means this process is already a group leader -- which init is, as
     * PID 1 -- and then it is already the leader of a session, so there is nothing to do. */
    if (setsid() < 0 && errno != EPERM) {
        say("[sbin/init] setsid failed (");
        say(strerror(errno));
        say("); the terminal is not a session of its own\n");
    }

    /* --- the shell, over and over ------------------------------------------------ */

    int respawns = 0;

    for (;;) {
        pid_t pid = fork();

        if (pid < 0) {
            /* Out of processes.  Retrying immediately would spin, so this waits for a
             * second and tries again -- a `fork` that fails because the task table is full
             * is usually a temporary condition, and a shell that comes up thirty seconds
             * late is worth more than a panic. */
            say("[sbin/init] fork failed: ");
            say(strerror(errno));
            say("; retrying\n");
            sleep(1);
            continue;
        }

        if (pid == 0)
            become_shell();

        shell_pid = pid;

        /* Wait for *any* child, not for this one specifically.
         *
         * That is the whole of orphan reaping and it is why this loop cannot be written as
         * `while (wait(&status) > 0)` around a single shell: a background job the shell
         * started, whose own shell has since died, is re-parented to PID 1 by the kernel
         * and shows up here.  Waiting for the shell by pid would leave those until the
         * shell happened to exit, and an orphaned `sleep 100` would then be unreapable for
         * a hundred seconds. */
        int status = 0;
        pid_t done = waitpid(-1, &status, 0);

        if (done < 0) {
            if (errno == EINTR)
                continue;       /* a signal arrived; there is still a child to wait for */

            if (errno == ECHILD)
                break;          /* nothing left at all: nothing to supervise */

            say("[sbin/init] wait failed: ");
            say(strerror(errno));
            say("\n");
            continue;
        }

        /* Somebody that is not the shell.  Reaped and forgotten: the kernel has already
         * moved it here, and holding on to it would be holding on to a dead task. */
        if (done != shell_pid) {
            char buf[8];
            int  n = 0;
            long v = (long)done;

            /* Its id, so that a person watching a machine with a leak can see the orphans
             * being collected rather than having to take it on trust. */
            buf[n++] = '[';

            char num[16];
            int  m = 0;

            if (v == 0)
                num[m++] = '0';

            while (v > 0 && m < (int)sizeof(num)) {
                num[m++] = (char)('0' + (v % 10));
                v /= 10;
            }

            for (int i = m - 1; i >= 0; i--)
                buf[n++] = num[i];

            buf[n++] = ']';
            buf[n++] = '\n';

            ssize_t ignored = write(1, buf, (size_t)n);

            (void)ignored;
            continue;
        }

        /* The shell. */
        shell_pid = -1;

        if (WIFSIGNALED(status)) {
            say("[sbin/init] the shell was killed by signal ");
            say_num((long)WTERMSIG(status));
            say("\n");
        } else {
            say("[sbin/init] the shell exited with status ");
            say_num((long)WEXITSTATUS(status));
            say("\n");
        }

        if (shell_exit_is_final(status)) {
            say("[sbin/init] nothing left to run; stopping the machine\n");
            sync();
            /* `RB_AUTOBOOT` is the only value that reboots; every other value powers off,
             * and stopping is the right answer for a machine whose shell has just exited on
             * purpose. */
            reboot(0);
            /* Reached only if the kernel declined.  Then the machine has no way to stop,
             * so the honest thing is to say so and spin rather than exit and leave PID 1
             * gone: a machine with no init has nothing to reap and nothing to restart. */
            say("[sbin/init] the kernel would not stop the machine\n");

            for (;;)
                pause();
        }

        respawns++;

        if (respawns >= SHELL_RESPAWN_LIMIT) {
            say("[sbin/init] the shell exited ");
            say_num((long)respawns);
            say(" times in a row; stopping rather than looping\n");
            sync();
            reboot(0);

            for (;;)
                pause();
        }
    }

    /* `ECHILD` with no shell to have run: every child init had has gone and none of them
     * was ever a shell.  Same ending as the shell exiting. */
    say("[sbin/init] no children left; stopping the machine\n");
    sync();
    reboot(0);

    for (;;)
        pause();

    return 0;
}
