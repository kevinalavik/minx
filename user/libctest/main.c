/* minx - libctest: what the libc actually does.
 *
 * Every other test program in `user/` is written against the raw door, on purpose: a test of
 * the kernel's syscall handling, its loader or its signal trampoline has to have nothing in
 * between that could be the bug.  This one is the opposite, deliberately.  It links libc,
 * it is written against `<stdio.h>` and `<unistd.h>`, and it is the only test here that
 * fails when `printf()` pads wrongly or `malloc()` fragments a heap.
 *
 * The cases are the ones whose failures are quiet:
 *
 *   - `printf()`'s width, precision, flags and length modifiers, checked against the exact
 *     bytes rather than by eye.  `%05d` of -42 is `-0042`, not `-4200` and not `  -42`; a
 *     `printf` that gets that wrong still prints something.
 *   - `snprintf()`'s return value, which is the number of characters it *would* have
 *     written.  A program that sizes a buffer with it is broken in a way that only shows up
 *     when the formatted output happens to be longer than the buffer.
 *   - `malloc()`/`free()` round trips, including the same size twice getting the same
 *     pointer, and a large allocation after the heap has been churned.
 *   - The `std` streams going to the descriptor they are supposed to, and a stream opened
 *     on a file in the filesystem.
 *
 * The return is zero only if everything passed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what) {
    if (ok) {
        printf("libctest: ok -- %s\n", what);
        return;
    }

    printf("libctest: FAILED -- %s\n", what);
    failures++;
}

/* Format into a buffer and compare the whole thing, byte for byte.
 *
 * Comparing with `strcmp` and not with "does it look right" is the point: the interesting
 * failures are a space where a zero should be and a digit that is one too many, and both
 * of those are invisible in a terminal and obvious to `strcmp`. */
static void check_fmt(const char *want, const char *what, const char *fmt, ...) {
    char    got[256];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(got, sizeof(got), fmt, ap);
    va_end(ap);

    if (n != (int)strlen(want) || strcmp(got, want) != 0) {
        printf("libctest: FAILED -- %s\n", want);
        printf("libctest:          got \"%s\" (%d)\n", got, n);
        failures++;
        return;
    }

    printf("libctest: ok -- %s\n", what);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;

    /* Unbuffered from the first line.
     *
     * This program reports as it goes, and a report that is still in a buffer when the
     * program dies is a report nobody reads -- which is exactly what happened the first
     * time this ran: it crashed after a dozen checks and the log had none of them, because
     * every line was sitting in stdout waiting for a flush that a faulting process never
     * reaches.  A test program wants its output *now*; a program that has to survive being
     * killed does not, which is why this is a decision made here and not in the libc. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- printf ------------------------------------------------------------ */

    check_fmt("hello", "a plain string", "%s", "hello");
    check_fmt("42", "an integer", "%d", 42);
    check_fmt("-42", "a negative integer", "%d", -42);
    check_fmt("  42", "a right-aligned width", "%4d", 42);
    check_fmt("42  ", "a left-aligned width", "%-4d", 42);
    check_fmt("00042", "zero padding", "%05d", 42);

    /* The one that catches the sign-before-padding bug: the zeroes go after the sign, so
     * this is `-0042` and five characters.  An implementation that zero-pads the magnitude
     * produces `-4200`, which is the same length and the wrong number. */
    check_fmt("-0042", "zero padding with a sign", "%05d", -42);
    check_fmt("+42", "an explicit plus", "%+d", 42);
    check_fmt(" 42", "a space instead of a plus", "% d", 42);
    check_fmt("2a", "hexadecimal", "%x", 42);
    check_fmt("2A", "hexadecimal, upper case", "%X", 42);
    check_fmt("0x2a", "hexadecimal with the prefix", "%#x", 42);
    check_fmt("0x002a", "hexadecimal, prefixed and padded", "%#06x", 42);
    check_fmt("52", "octal", "%o", 42);
    check_fmt("  ", "a width with an empty string", "%2s", "");
    check_fmt("ab", "a precision that truncates", "%.2s", "abcdef");
    check_fmt("", "a precision of zero on an empty string", "%.0s", "abc");

    /* `%.0d` of zero is *nothing*, on every other libc.  A `printf` that prints "0" here is
     * not wrong-looking; it is wrong, and it is wrong for `%.0f` of 0.0 too, which is how a
     * program prints an empty field rather than a "0" nobody asked for. */
    check_fmt("", "a precision of zero on zero", "%.0d", 0);
    check_fmt("1234567890123", "a long long", "%lld", 1234567890123LL);
    check_fmt("0x007fffffffffffff", "a pointer at a constant width", "%p",
             (void *)(uintptr_t)0x7fffffffffffffUL);
    check_fmt("%", "a literal percent", "%%");
    check_fmt("a1b2", "adjacent conversions", "a%db%d", 1, 2);

    /* `snprintf()`'s return is the length it would have written, which is the number a
     * program uses to size a buffer.  Getting it to return what it *did* write makes every
     * program that grows a buffer by one think the string was short by one, every time. */
    {
        char small[8];
        int  n = snprintf(small, sizeof(small), "%s", "a much longer string than fits");

        check(n == 30 && small[7] == 0,
              "snprintf returns the length it wanted, not the length it wrote");
    }

    /* --- malloc ------------------------------------------------------------- */

    {
        void *a = malloc(64);

        check(a != NULL, "malloc of 64 bytes");
        free(a);

        void *b = malloc(64);
        free(b);

        check(a == b, "free and malloc of the same size give the same pointer back");

        void *c = calloc(10, 8);

        check(c != NULL, "calloc");

        int zero = 1;

        for (int i = 0; i < 80; i++)
            if (((unsigned char *)c)[i] != 0)
                zero = 0;

        check(zero, "calloc returns zeroes, not whatever was there");

        /* Churn: allocate and free a thousand times and make sure the heap is still there.
         * A free list that does not coalesce fragments its own heap, and this is where that
         * shows up -- not as a wrong answer but as a machine that runs out of memory. */
        for (int i = 0; i < 1000; i++) {
            void *p = malloc(64 + (i % 7) * 16);

            if (p == NULL) {
                check(0, "the heap survives a thousand alloc/free rounds");
                break;
            }

            memset(p, i, 64);
            free(p);
        }

        void *big = malloc(256 * 1024);

        check(big != NULL, "a quarter-megabyte allocation after the churn");
        free(big);
        free(c);

        check(1, "the heap survives a thousand alloc/free rounds");
    }

    /* --- the environment ----------------------------------------------------- */

    {
        const char *path = getenv("PATH");

        check(path != NULL && path[0] != 0, "PATH is set before main()");

        setenv("LIBC_TEST", "yes", 1);
        check(strcmp(getenv("LIBC_TEST"), "yes") == 0, "setenv then getenv");

        setenv("LIBC_TEST", "no", 0);
        check(strcmp(getenv("LIBC_TEST"), "yes") == 0, "setenv does not overwrite unless asked");

        unsetenv("LIBC_TEST");
        check(getenv("LIBC_TEST") == NULL, "unsetenv");
    }

    /* --- strings -------------------------------------------------------------- */

    {
        check(strlen("hello") == 5, "strlen");
        check(strcmp("abc", "abd") < 0, "strcmp orders by byte value");
        check(strcmp("abd", "abc") > 0, "strcmp is antisymmetric");
        check(strcmp("abc", "abc") == 0, "strcmp on equal strings");
        check(memcmp("\xff", "\x01", 1) > 0, "memcmp compares bytes as unsigned");

        char buf[16];

        strcpy(buf, "abc");
        strcat(buf, "def");
        check(strcmp(buf, "abcdef") == 0, "strcpy then strcat");

        check(strchr("hello", 'l') != NULL && *strchr("hello", 'l') == 'l', "strchr finds a letter");
        check(strchr("hello", 0) != NULL, "strchr of NUL is the terminator, not NULL");
        check(strrchr("hello", 'l')[1] == 'o', "strrchr finds the last one");

        check(strstr("hello world", "o w") != NULL, "strstr finds a substring");
        check(strstr("hello", "zz") == NULL, "strstr of something absent is NULL");

        char *dup = strdup("duplicated");
        check(dup != NULL && strcmp(dup, "duplicated") == 0, "strdup");
        free(dup);

        /* Overlapping copies: `memmove` is defined for them and `memcpy` is not, so this is
         * the check that the two are actually different functions. */
        char mv[16] = "abcdefgh";
        memmove(mv + 2, mv, 6);
        check(memcmp(mv, "ababcdef", 8) == 0, "memmove with the destination above the source");
    }

    /* --- errno ----------------------------------------------------------------- */

    {
        errno = 0;
        int fd = open("/definitely/not/here", O_RDONLY);

        check(fd < 0 && errno == ENOENT, "open of a missing file sets ENOENT");

        if (fd >= 0)
            close(fd);

        errno = 0;
        close(999);
        check(errno == EBADF, "close of a descriptor that is not open sets EBADF");

        const char *msg = strerror(ENOENT);

        check(msg != NULL && msg[0] != 0, "strerror gives a name for an error");
    }

    /* --- the filesystem, through libc ------------------------------------------- */

    {
        unlink("/tmp/libctest");

        FILE *f = fopen("/tmp/libctest", "w");

        check(f != NULL, "fopen for writing");

        if (f != NULL) {
            fprintf(f, "one\ntwo\nthree\n");
            fclose(f);

            f = fopen("/tmp/libctest", "r");
            check(f != NULL, "fopen for reading");

            if (f != NULL) {
                char line[64];

                check(fgets(line, sizeof(line), f) != NULL &&
                      strcmp(line, "one\n") == 0, "fgets reads the first line");
                check(fgets(line, sizeof(line), f) != NULL &&
                      strcmp(line, "two\n") == 0, "fgets reads the second line");
                check(fgets(line, sizeof(line), f) != NULL &&
                      strcmp(line, "three\n") == 0, "fgets reads the third line");
                check(fgets(line, sizeof(line), f) == NULL, "fgets returns NULL at end of file");
                check(feof(f), "the stream knows it is at end of file");
                fclose(f);
            }

            struct stat st;

            check(stat("/tmp/libctest", &st) == 0, "stat");
            check(st.st_size == 14, "stat reports the file's length");
            check(S_ISREG(st.st_mode), "S_ISREG on a regular file");
            check(!S_ISDIR(st.st_mode), "a regular file is not a directory");

            struct stat dst;

            check(stat("/tmp", &dst) == 0 && S_ISDIR(dst.st_mode),
                  "S_ISDIR on a directory");

            unlink("/tmp/libctest");
            check(stat("/tmp/libctest", &st) == -1 && errno == ENOENT,
                  "unlink, and the file is gone");
        }
    }

    /* --- setjmp -------------------------------------------------------------- */

    {
        /* The jump has to come back with its own value, and the value is the only way the
         * code after the jump can tell which path it took.  A `longjmp` that arrives as 0
         * would be indistinguishable from the `setjmp` that saved the environment. */
        static jmp_buf env;
        volatile int reached = 0;
        int v = setjmp(env);

        if (v == 0) {
            reached = 1;
            longjmp(env, 42);
            check(0, "longjmp did not return at all");
        }

        check(v == 42 && reached == 1, "setjmp returns 0 and longjmp returns its value");

        /* The registers, not just the control flow: a `longjmp` that restores the stack
         * but not r12..r15 comes back with the *caller's* values in them, which is a
         * corruption that only shows up if something is in one. */
        static jmp_buf env2;
        volatile unsigned long kept = 0;
        register unsigned long r12 asm("r12") = 0x5a5a5a5a5a5a5a5aull;

        if (setjmp(env2) == 0)
            longjmp(env2, 7);

        kept = r12;
        check(kept == 0x5a5a5a5a5a5a5a5aull, "longjmp restores the callee-saved registers");
    }

    /* --- directories ----------------------------------------------------------- */

    {
        mkdir("/tmp/libctest-dir", 0755);

        FILE *mk = fopen("/tmp/libctest-dir/one", "w");
        if (mk != NULL) { fputs("x", mk); fclose(mk); }
        mk = fopen("/tmp/libctest-dir/two", "w");
        if (mk != NULL) { fputs("y", mk); fclose(mk); }
        mkdir("/tmp/libctest-dir/inner", 0755);

        DIR *d = opendir("/tmp/libctest-dir");

        check(d != NULL, "opendir");

        if (d != NULL) {
            int saw_dot = 0, saw_dotdot = 0, saw_one = 0, saw_two = 0, saw_inner = 0;
            int saw_reg = 0, saw_dir = 0, entries = 0;
            struct dirent *e;

            /* `.` and `..` are returned by every Unix and `find` cannot terminate without
             * `..`, so a libc that hid them would be hiding the end of the walk. */
            while ((e = readdir(d)) != NULL) {
                entries++;

                if (strcmp(e->d_name, ".") == 0)       saw_dot++;
                else if (strcmp(e->d_name, "..") == 0) saw_dotdot++;
                else if (strcmp(e->d_name, "one") == 0) {
                    saw_one++;
                    saw_reg = (e->d_type == DT_REG);
                } else if (strcmp(e->d_name, "two") == 0) {
                    saw_two++;
                } else if (strcmp(e->d_name, "inner") == 0) {
                    saw_inner++;
                    saw_dir = (e->d_type == DT_DIR);
                }
            }

            check(entries == 5, "readdir lists every entry, including . and ..");
            check(saw_dot == 1 && saw_dotdot == 1, "readdir returns . and .. exactly once");
            check(saw_one == 1 && saw_two == 1 && saw_inner == 1,
                  "readdir lists each file");
            check(saw_reg, "d_type says a regular file is DT_REG");
            check(saw_dir, "d_type says a directory is DT_DIR");
            check(readdir(d) == NULL, "readdir returns NULL at the end of the directory");

            /* Rewinding, because `find` does it and because a directory walk that cannot
             * go back is a walk a program can only make once. */
            rewinddir(d);
            check(readdir(d) != NULL, "rewinddir puts the walk back at the start");

            check(closedir(d) == 0, "closedir");
        }

        /* And the directory is gone once everything in it is. */
        unlink("/tmp/libctest-dir/one");
        unlink("/tmp/libctest-dir/two");
        rmdir("/tmp/libctest-dir/inner");
        check(rmdir("/tmp/libctest-dir") == 0, "the test directory is removed");
    }

    /* --- getopt ---------------------------------------------------------------- */

    {
        /* The parsing rules are the whole of `getopt`, and three of them are not what a
         * newcomer expects: options stop at the first non-option, `--` ends them, and a
         * missing argument is a different answer from an unknown option. */
        /* `-bc` is two options in one element, and `-d arg` is one option with the next
         * element as its argument.  Both spellings have to work and neither may eat the
         * other: `-bV` with a `b` that takes an argument is `-b V`, not `-b -V`. */
        static char *av1[] = { "prog", "-a", "-bc", "-d", "arg", "file", "--", "-z", NULL };
        static char *av2[] = { "prog", "-q", NULL };

        opterr = 0;
        optind = 1;
        optreset();

        int c = getopt(8, av1, "abcd:");
        check(c == 'a', "getopt returns a short option");

        c = getopt(8, av1, "abcd:");
        check(c == 'b', "getopt reads the first option of a bundled element");

        c = getopt(8, av1, "abcd:");
        check(c == 'c', "getopt reads several options out of one element");

        c = getopt(8, av1, "abcd:");
        check(c == 'd' && optarg != NULL && strcmp(optarg, "arg") == 0,
              "getopt takes a separate argument and puts it in optarg");

        /* `file` is not an option, so the options are over -- and the `-z` after the `--`
         * must not be reached, or a program could never name a file that starts with a
         * dash. */
        c = getopt(8, av1, "abcd:");
        check(c == -1 && optind == 5,
              "getopt stops at the first non-option and leaves optind on it");

        /* An unknown option is reported and skipped, not treated as the end: a program
         * that gave up could never be given a new option by whoever invoked it. */
        optreset();
        c = getopt(2, av2, "ab");
        check(c == '?', "getopt reports an unknown option");

        /* And a *known* option that wanted an argument and did not get one, with the
         * leading colon in optstring so the two cases can be told apart. */
        static char *av3[] = { "prog", "-b", NULL };

        optreset();
        c = getopt(2, av3, ":ab:");
        check(c == ':' && optopt == 'b',
              "getopt distinguishes a missing argument from an unknown option");

        /* The attached spelling, which is how `-Wall` works. */
        static char *av4[] = { "prog", "-bV", NULL };

        optreset();
        c = getopt(2, av4, "b:");
        check(c == 'b' && optarg != NULL && strcmp(optarg, "V") == 0,
              "getopt takes an attached argument");
    }

    /* --- fnmatch ---------------------------------------------------------------- */

    {
        check(fnmatch("*", "anything", 0) == 0, "fnmatch: * matches");
        check(fnmatch("*.c", "hello.c", 0) == 0, "fnmatch: * at the end");
        check(fnmatch("*.c", "hello.o", 0) == FNM_NOMATCH, "fnmatch: * at the end, no match");
        check(fnmatch("a?c", "abc", 0) == 0, "fnmatch: ? is one character");
        check(fnmatch("a?c", "ac", 0) == FNM_NOMATCH, "fnmatch: ? is exactly one");
        check(fnmatch("a[bc]d", "abd", 0) == 0, "fnmatch: a set");
        check(fnmatch("a[!bc]d", "abd", 0) == FNM_NOMATCH, "fnmatch: a negated set");
        check(fnmatch("a[!bc]d", "axd", 0) == 0, "fnmatch: a negated set matches");
        check(fnmatch("[a-z]*", "hello", 0) == 0, "fnmatch: a range");
        check(fnmatch("a\\*b", "a*b", 0) == 0, "fnmatch: a backslash escapes");
        check(fnmatch("a\\*b", "axxb", 0) == FNM_NOMATCH, "fnmatch: an escaped star is literal");

        /* The leading dot, which `FNM_PERIOD` is about and `FNM_PATHNAME` is not.  Note
         * that the *default* lets `*` match a leading dot: that is POSIX, and it is why
         * `glob()` below always passes the flag and why `sh` never offers a dotfile to
         * the matcher at all.  A libc whose default were the other way would make
         * `fnmatch("*", ".hidden", 0)` a test that passes here and fails everywhere. */
        check(fnmatch("*", ".hidden", 0) == 0,
              "fnmatch: * matches a leading dot unless FNM_PERIOD says otherwise");
        check(fnmatch("*", ".hidden", FNM_PERIOD) == FNM_NOMATCH,
              "fnmatch: FNM_PERIOD stops it");
        check(fnmatch(".*", ".hidden", 0) == 0, "fnmatch: .* matches a leading dot");
        check(fnmatch("?", ".hidden", FNM_PERIOD) == FNM_NOMATCH,
              "fnmatch: FNM_PERIOD stops ? as well as *");

        /* And the one that stops `*` crossing a directory boundary, which is what makes
         * `find -name '*.c'` find files rather than directories that end in .c. */
        check(fnmatch("*", "a/b", 0) == 0, "fnmatch: * crosses a slash by default");
        check(fnmatch("*", "a/b", FNM_PATHNAME) == FNM_NOMATCH,
              "fnmatch: FNM_PATHNAME stops it");
        check(fnmatch("src/*.c", "src/a.c", FNM_PATHNAME) == 0,
              "fnmatch: a pattern with a directory");
        check(fnmatch("*.c", "src/a.c", FNM_PATHNAME) == FNM_NOMATCH,
              "fnmatch: FNM_PATHNAME does not let a pattern match a path");

        /* A `[` that is never closed is a literal bracket, not an unterminated set: a
         * pattern that treated it as a set would match `[` itself and change the meaning
         * of an ordinary file name. */
        check(fnmatch("[abc", "[abc", 0) == 0, "fnmatch: an unterminated set is literal");
    }

    /* --- glob ------------------------------------------------------------------- */

    {
        mkdir("/tmp/globtest", 0755);

        static const char *names[] = { "one.c", "two.c", "three.h", ".hidden", NULL };

        for (int i = 0; names[i] != NULL; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/globtest/%s", names[i]);
            FILE *f = fopen(path, "w");
            if (f != NULL)
                fclose(f);
        }

        glob_t g;

        memset(&g, 0, sizeof(g));
        check(glob("/tmp/globtest/*.c", 0, NULL, &g) == 0 && g.gl_pathc == 2,
              "glob finds the two .c files");
        check(g.gl_pathc == 2 && strcmp(g.gl_pathv[0], "/tmp/globtest/one.c") == 0 &&
              strcmp(g.gl_pathv[1], "/tmp/globtest/two.c") == 0,
              "glob sorts what it found, and only what matched");
        globfree(&g);

        /* A dotfile is not matched by `*`, which is the difference between `cp *` and
         * `cp .*`. */
        memset(&g, 0, sizeof(g));
        check(glob("/tmp/globtest/*", 0, NULL, &g) == 0 && g.gl_pathc == 3,
              "glob does not hand back a dotfile for *");
        globfree(&g);

        memset(&g, 0, sizeof(g));
        check(glob("/tmp/globtest/.*", 0, NULL, &g) == 0 && g.gl_pathc >= 2,
              "glob finds a dotfile when the pattern says so");
        globfree(&g);

        /* Nothing matching is not an *error*, and it is still not a success: the answer is
         * `GLOB_NOMATCH`, which is nonzero, which is the only way a caller can tell "no
         * files" from "something went wrong".  That is why `GLOB_NOMATCH` is 3 and not 0.
         */
        memset(&g, 0, sizeof(g));
        check(GLOB_NOMATCH != 0, "GLOB_NOMATCH is not the same value as success");
        memset(&g, 0, sizeof(g));
        check(glob("/tmp/globtest/*.zzz", 0, NULL, &g) == GLOB_NOMATCH && g.gl_pathc == 0,
              "glob reports no match without calling it an error");
        globfree(&g);

        /* With GLOB_NOCHECK the pattern comes back unchanged, which is what lets `rm *.o`
         * in a directory with no object files say which name it could not find -- and it is
         * a *success*, because the caller asked for the pattern back and got it. */
        memset(&g, 0, sizeof(g));
        check(glob("/tmp/globtest/*.zzz", GLOB_NOCHECK, NULL, &g) == 0 &&
              g.gl_pathc == 1 && strcmp(g.gl_pathv[0], "/tmp/globtest/*.zzz") == 0,
              "glob with GLOB_NOCHECK returns the pattern itself");
        globfree(&g);

        /* A pattern in a directory that is not there is "no match", not an error, or a
         * glob under a missing directory could not be reported. */
        memset(&g, 0, sizeof(g));
        check(glob("/tmp/no-such-dir/*", 0, NULL, &g) == GLOB_NOMATCH && g.gl_pathc == 0,
              "glob under a missing directory is no match, not an error");

        for (int i = 0; names[i] != NULL; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/globtest/%s", names[i]);
            unlink(path);
        }
        check(rmdir("/tmp/globtest") == 0, "the glob test directory is removed");
    }

    /* --- the standard streams -------------------------------------------------- */

    {
        /* A stream on a pipe, which is the shape every command in a pipeline has.  It is
         * here because it is the only way to check that `fflush()` does what a program
         * between two commands depends on: `echo hello | cat` has to see the bytes before
         * the writing process exits. */
        int fds[2];

        check(pipe(fds) == 0, "pipe");

        if (fds[0] >= 0) {
            FILE *w = fdopen(fds[1], "w");

            check(w != NULL, "fdopen on the writing end");

            if (w != NULL) {
                fprintf(w, "through a pipe\n");
                check(fflush(w) == 0, "fflush on a buffered stream");
                fclose(w);
            }

            char buf[32];
            ssize_t n = read(fds[0], buf, sizeof(buf) - 1);

            if (n > 0)
                buf[n] = 0;

            check(n == 15 && strcmp(buf, "through a pipe\n") == 0,
                  "the other end of the pipe saw what was written");
            close(fds[0]);
        }
    }

    /* --- the exit status -------------------------------------------------------- */

    printf("libctest: %s\n", failures == 0 ? "ok" : "FAILED");

    /* The count, not a yes/no.  A test that reports "ok" for one failed check out of
     * forty is worse than one that reports how many, because a reader cannot tell the two
     * apart without reading the whole list. */
    return failures == 0 ? 0 : 1;
}