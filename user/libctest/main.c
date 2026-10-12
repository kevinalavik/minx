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