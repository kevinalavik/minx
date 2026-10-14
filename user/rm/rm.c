/* `rm` -- remove files.
 *
 * One of the six programs `shtest`'s script needs: `rm -f /tmp/shtest-out` cleans up between
 * the append test and the line count, so that the count is of what the script wrote and not
 * of what a previous run left behind.
 *
 * Directories are *not* removed, and asking is a refusal with a message rather than a
 * silent no-op: `rm -r` is a different command and this does not implement it, so a script
 * that uses it gets told rather than getting a partial result.
 *
 * `-f` is honoured for what it is for -- a missing file is not an error -- and nothing else.
 * A *present* file that cannot be removed is always an error, because `-f`'s job on every
 * platform is to silence the *missing* case only.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
int  nargs = (int)argc;    /* `argc` arrives as an `unsigned long` because that is what the entry stub passes, and
     * every loop index here is an `int`.  They are the same value on this ABI; the cast
     * says so once rather than at every comparison, and says it because a `-Wsign-compare`
     * that has been silenced in the build is a real class of bug papered over rather than
     * answered. */
    
    /*  is  because that is what the entry stub passes, and comparing
     * it against an  index is a sign-compare warning that is right to be noisy about:
     * the two are the same on this ABI and the cast is here to say so once. */
    int  force = 0;
    int  status = 0;
    int  i = 1;

    for (; i < nargs; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            force = 1;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != 0) {
            /* Said plainly rather than refused as an unknown option: `-r` is a real thing
             * people write, and a program that answers "not implemented" is more use than
             * one that answers "unknown". */
            fprintf(stderr, "rm: %s is not implemented\n", argv[i]);
            return 1;
        }

        break;
    }

    if (i >= nargs) {
        fprintf(stderr, "rm: no file named\n");
        return 1;
    }

    for (; i < nargs; i++) {
        struct stat st;

        if (stat(argv[i], &st) != 0) {
            if (errno == ENOENT && force)
                continue;

            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            fprintf(stderr, "rm: %s: is a directory\n", argv[i]);
            status = 1;
            continue;
        }

        if (unlink(argv[i]) != 0) {
            if (errno == ENOENT && force)
                continue;

            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }

    return status;
}