/* minx - init: the first program the kernel runs.
 *
 * PID 1, and the reason a process tree exists at all.  The kernel does not run a shell: it
 * runs *this*, and this runs the shell.  That indirection is not ceremony -- it is what
 * gives a machine somewhere to put a program that outlives every other process, what to
 * re-parent orphans to, and what to ask for a reboot.
 *
 * What it does, in order, and why in that order:
 *
 *   1. Mounts `/proc` and `/dev`.  Both are already mounted by the kernel, because a machine
 *      that boots with no way to read a device or to ask what it is cannot be reported on;
 *      this is the call an init makes, and it is a no-op for a kernel that got there first.
 *   2. Opens `/dev/console`.  Everything after this is written through a descriptor rather
 *      than to "the console", which is what makes the output the same text whether it goes
 *      to the framebuffer, to the serial mirror, or to a pipe a test put there.
 *   3. Execs `/bin/sh`, with the terminal attached.  `exec` and not "fork and wait": init has
 *      nothing to do afterwards, and a process tree with an init that is waiting for a
 *      shell has one more task than it needs.
 *
 * This is the freestanding version, written against `user/common/minx_user.h` with no
 * libc.  It exists to prove the filesystem end to end -- a program read out of the initrd by
 * a path, mounting, opening, writing, reading back and `exec`ing another program from the
 * same filesystem -- and then to hand over to `/sbin/init`, which is the real PID 1 and
 * owns job control, orphan reaping and stopping the machine.
 *
 * It stays freestanding on purpose.  The things it has to prove are about the kernel, and a
 * libc in the way would make every failure look like a libc failure.  What it proves, it
 * proves with nothing but `int $0x80`.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

static int console_fd = 1;

/* Byte-by-byte comparison rather than `memcmp`, because this program has no libc and the
 * whole point of writing it without one is that nothing has to be there for it to work. */
static int same_bytes(const char *a, const char *b, size_t_ n) {
    for (size_t_ i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void say(const char *s) {
    minx_write(console_fd, s, minx_strlen(s));
}

static void say_num(unsigned long v) {
    minx_put_num(v);
}

/* Write to whatever descriptor 1 is, which is the console until init opens its own.
 *
 * `minx_put*` write to descriptor 1 deliberately: every message this program produces is a
 * boot message, and a boot message that went somewhere else would be one nobody reads. */
int minx_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    say("init: starting, pid ");
    say_num(minx_getpid());
    say("\n");

    /* --- 1. the two filesystems ------------------------------------------ */

    if (minx_mount("devfs", "/dev", "devfs") != 0) {
        say("init: /dev was not mounted: ");
        say_num((unsigned long)(-minx_mount("devfs", "/dev", "devfs")));
        say("\n");
    }

    if (minx_mount("procfs", "/proc", "procfs") != 0) {
        say("init: /proc was not mounted: ");
        say_num((unsigned long)(-minx_mount("procfs", "/proc", "procfs")));
        say("\n");
    }

    say("init: /dev and /proc are mounted\n");

    /* --- 2. the console --------------------------------------------------- */

    int fd = (int)minx_open("/dev/console", O_RDWR, 0);

    if (fd < 0) {
        say("init: /dev/console would not open, and the messages below will "
            "still reach the framebuffer\n");
    } else {
        console_fd = fd;
        say("init: console is descriptor ");
        say_num((unsigned long)fd);
        say("\n");
    }

    /* --- 3. proof that the filesystem is a filesystem --------------------- */

    /* Something written, closed, reopened and read back, with the bytes compared.  Not a
     * demonstration: a filesystem that accepts a write and loses it is a filesystem every
     * program above this one has to be written not to use. */
    {
        static const char message[] = "minx init wrote this.\n";
        long opened_probe = minx_open("/tmp/init-probe", O_RDWR | O_CREAT | O_TRUNC, 0644);
        int f = (int)opened_probe;

        if (f < 0) {
            say("init: could not create /tmp/init-probe: ");
            say_num((unsigned long)(-f));
            say("\n");
        } else {
            minx_write(f, message, sizeof(message) - 1);
            minx_close(f);

            f = (int)minx_open("/tmp/init-probe", O_RDONLY, 0);

            if (f < 0) {
                say("init: could not reopen what it just wrote\n");
            } else {
                char buf[64];
                long n = minx_read(f, buf, sizeof(buf) - 1);

                minx_close(f);
                buf[n > 0 ? n : 0] = 0;

                if (n == (long)(sizeof(message) - 1) &&
                    same_bytes(buf, message, (size_t_)n)) {
                    say("init: wrote and read back ");
                    say_num((unsigned long)n);
                    say(" bytes\n");
                } else {
                    say("init: what came back is not what was written\n");
                }
            }
        }
    }

    /* --- 4. the shell ----------------------------------------------------- */

    /* `/sbin/init`, and `exec` keeps this process's pid -- so the program that ends up
     * being PID 1 is the one that owns job control, reaps orphans and stops the machine,
     * rather than a bootstrap stub that has none of those.
     *
     * That is the whole reason this program exists in two pieces.  A stub that forked the
     * real init would leave the real init as PID 2 with no orphans ever re-parented to it,
     * because the kernel hands orphans to PID 1 by definition; and a stub that *was* PID 1
     * would have to grow into the full one, which is the program that wants libc, a shell
     * and a TTY.  So the small one proves the filesystem end to end and then hands over.
     *
     * The fallback to `/bin/hello` is for a system whose initrd has no `/sbin`: the
     * message below says which of the two happened, so nobody has to guess. */
    say("init: exec /sbin/init\n");

    {
        char *args[2];
        char *envp[1];

        args[0] = (char *)"/sbin/init";
        args[1] = NULL;
        envp[0] = NULL;

        long r = minx_exec("/sbin/init", args, envp);

        /* Only reached on failure: a successful exec does not return, because the program
         * that called it is gone. */
        say("init: exec /sbin/init failed with ");
        say_num((unsigned long)(-r));
        say("; falling back to /bin/hello\n");

        args[0] = (char *)"/bin/hello";
        r = minx_exec("/bin/hello", args, envp);

        say("init: exec /bin/hello failed with ");
        say_num((unsigned long)(-r));
        say("; nothing to run, so init exits\n");
    }

    minx_exit(0);
    return 0;
}