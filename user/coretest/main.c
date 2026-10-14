/* `coretest` -- an automated test for every coreutils program.
 *
 * The spec asks for one automated test per tool, and `libctest` is what the libc got.  This
 * is the same idea for the programs: each is run with arguments and an input, its output is
 * compared byte for byte against what it should be, and its exit status is checked.
 *
 * **How a program is run from inside a program.**
 *
 * There is no `system()` here and there will not be one: each test forks, `execve()`s the
 * tool with its argument vector and an input pipe, and reads the answer back through
 * another.  That is four syscalls more than `system()` would need and it is the only way to
 * do it, because `system()` is a shell, and a test for `ls` that runs `ls` through a shell
 * is a test that passes when the shell is wrong.
 *
 * Both pipes are read before the child is waited for, for the reason `sh_run_capture()`
 * gives: a child that writes more than a pipe buffer would block on the write, and a parent
 * that waited first would wait for ever.
 *
 * The expected output is compared including its trailing newline.  It has to be: a tool that
 * drops the last newline is a tool whose output cannot be concatenated with anything, and
 * comparing with the newline stripped would not notice.
 *
 * Every check names its tool and what it was checking, because a failure that says only
 * "output mismatch" makes the reader go and find out which of forty checks it was.
 */
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXARGS 8

/* One test.
 *
 * `argv` is NULL-terminated and excludes `argv[0]`, which is `tool`, so a line in the table
 * reads like the command a person would type: `{"-l", "/tmp", NULL}` is `ls -l /tmp`.
 *
 * `input` is what the tool reads from standard input, or NULL for nothing.  Tools that take
 * their input from a pipe are half of what these programs are *for* -- `tr`, `wc` and
 * `uniq` all exist to be the middle of a pipeline -- so a test that only ever gave them a
 * file name would not be testing them as they are used. */
typedef struct {
    const char *tool;
    const char *argv[MAXARGS];
    const char *input;
    const char *expected;
    const char *pattern;         /* fnmatch(), or NULL to compare `expected` byte for byte */
    int         status;          /* the exit status it should end with */
    const char *what;
} check_t;

static int checks;
static int failures;

static void fail(const char *tool, const char *what) {
    failures++;
    fprintf(stderr, "coretest: FAILED -- %s: %s\n", tool, what);
}

/* Read one end of a pipe to end-of-file.  Returns a NUL-terminated buffer the caller frees. */
static char *drain(int fd, size_t *len_out) {
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (buf == NULL)
        return NULL;

    for (;;) {
        if (len + 1 >= cap) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL)
                break;

            buf = grown;
            cap *= 2;
        }

        ssize_t n = read(fd, buf + len, cap - len - 1);

        if (n <= 0)
            break;

        len += (size_t)n;
    }

    buf[len] = 0;
    *len_out = len;
    return buf;
}

/* Write `s` to a pipe and close it, so the reader sees end-of-file. */
static void feed(int fd, const char *s) {
    size_t left = s != NULL ? strlen(s) : 0;

    while (left > 0) {
        ssize_t n = write(fd, s, left);

        if (n <= 0)
            break;

        s += n;
        left -= (size_t)n;
    }

    close(fd);
}

/* Run one tool.  Returns 0 if it ran, and fills in the rest. */
static int run_tool(const check_t *c, char **out, size_t *out_len, int *status) {
    int   ofds[2];
    int   ifds[2];
    pid_t pid;

    *out = NULL;
    *out_len = 0;
    *status = -1;

    if (pipe(ofds) != 0) {
        fail(c->tool, "pipe");
        return -1;
    }

    /* Only opened when there is something to write, so a test with no input gives the tool
     * an empty pipe rather than this program's own standard input -- which, in a kernel self
     * test, is the serial port, and a tool that read it would stop the whole run. */
    int have_in = c->input != NULL;

    if (have_in && pipe(ifds) != 0) {
        fail(c->tool, "pipe");
        close(ofds[0]);
        close(ofds[1]);
        return -1;
    }

    pid = fork();

    if (pid < 0) {
        fail(c->tool, "fork");
        close(ofds[0]);
        close(ofds[1]);

        if (have_in) {
            close(ifds[0]);
            close(ifds[1]);
        }

        return -1;
    }

    if (pid == 0) {
        char *argv[MAXARGS + 2];
        int   n = 0;

        close(ofds[0]);

        if (have_in) {
            close(ifds[1]);
            dup2(ifds[0], 0);
            close(ifds[0]);
        }

        dup2(ofds[1], 1);
        close(ofds[1]);

        argv[n++] = (char *)c->tool;

        for (int i = 0; i < MAXARGS && c->argv[i] != NULL; i++)
            argv[n++] = (char *)c->argv[i];

        argv[n] = NULL;

        /* `execvp()` and not `execve()`: `execve()` takes a *path*, so a bare name would be
         * looked for in the current directory and every one of these would fail with
         * ENOENT.  It is also the libc's own PATH search rather than a second one written
         * here.  127 is the status a shell reports for "could not run it", so a missing
         * tool is a clear failure rather than an empty output that looks like a pass. */
        execvp(c->tool, argv);

        fprintf(stderr, "coretest: cannot exec %s: %s\n", c->tool, strerror(errno));
        _exit(127);
    }

    close(ofds[1]);

    if (have_in) {
        close(ifds[0]);
        feed(ifds[1], c->input);
    }

    size_t  len = 0;
    char   *buf = drain(ofds[0], &len);

    close(ofds[0]);

    int st = 0;

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;

    *out = buf;
    *out_len = len;
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return 0;
}

/* Print a string so a mismatch is readable when the bytes are not printable.  Only the
 * characters that would break the report are escaped; everything else is passed through,
 * because escaping everything would make the expected line harder to read than the failure
 * it is reporting. */
static void show(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n')
            fputs("\\n", stderr);
        else if (c == '\t')
            fputs("\\t", stderr);
        else if (c == '\r')
            fputs("\\r", stderr);
        else if (c < 0x20)
            fprintf(stderr, "\\x%02x", c);
        else
            fputc((int)c, stderr);
    }
}

static void run_check(const check_t *c) {
    char  *out = NULL;
    size_t len = 0;
    /* Only the byte-for-byte form has an expected string.  The pattern form has `expected`
     * NULL, and passing that to `strlen()` is a fault -- which is what happened, on the first
     * check written in the new form, after forty checks had passed. */
    size_t want = c->expected != NULL ? strlen(c->expected) : 0;
    int    status = -1;

    checks++;

    if (run_tool(c, &out, &len, &status) != 0)
        return;

    if (out == NULL) {
        fail(c->tool, c->what);
        return;
    }

    /* The pattern form: the output's *shape* is checked and the parts that legitimately vary
     * between machines are not pinned.
     *
     * `fnmatch()` and not a regular expression, for the same reason `grep` has none -- a
     * partial pattern engine passes the wrong thing and fails the right one, silently.  A
     * timestamp or a uid in the output is a real reason for this, not a reason for a looser
     * test: pinning them would mean a check that passes on the machine that wrote it. */
    if (c->pattern != NULL) {
        if (fnmatch(c->pattern, out, 0) == 0 && status == c->status) {
            fprintf(stderr, "coretest: ok -- %s\n", c->what);
            free(out);
            return;
        }

        fprintf(stderr, "coretest: FAILED -- %s: %s\n", c->tool, c->what);
        fprintf(stderr, "  wanted to match: %s\n", c->pattern);
        fprintf(stderr, "  got (%lu bytes): ", (unsigned long)len);
        show(out, len);
        fprintf(stderr, "\n");

        if (status != c->status)
            fprintf(stderr, "  and it exited %d, expected %d\n", status, c->status);

        failures++;
        free(out);
        return;
    }

    if (len != want || memcmp(out, c->expected, len) != 0) {
        fprintf(stderr, "coretest: FAILED -- %s: %s\n", c->tool, c->what);
        fprintf(stderr, "  wanted (%lu bytes): ", (unsigned long)want);
        show(c->expected, want);
        fprintf(stderr, "\n  got    (%lu bytes): ", (unsigned long)len);
        show(out, len);
        fprintf(stderr, "\n");
        failures++;
        free(out);
        return;
    }

    if (status != c->status) {
        fprintf(stderr, "coretest: FAILED -- %s: %s: exited %d, expected %d\n",
                c->tool, c->what, status, c->status);
        failures++;
        free(out);
        return;
    }

    fprintf(stderr, "coretest: ok -- %s\n", c->what);
    free(out);
}

static int put_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return -1;

    size_t n = strlen(contents);

    if (n > 0 && fwrite(contents, 1, n, f) != n) {
        fclose(f);
        return -1;
    }

    return fclose(f) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* A directory of this program's own, so a test that creates a file cannot collide with
     * anything else and so the leftovers are identifiable if a test fails. */
    if (mkdir("/tmp/coretest", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "coretest: cannot make /tmp/coretest: %s\n", strerror(errno));
        return 1;
    }

    /* The fixtures first, and a fixture that cannot be made is itself a failure.  Otherwise
     * every check that reads it fails for a reason that is not the tool's, and a reader
     * spends the time working out which. */
    static const struct { const char *path; const char *contents; } fixtures[] = {
        { "/tmp/coretest/lsdir/cat.txt",  "one\ntwo\nthree\n" },
        { "/tmp/coretest/lsdir/head.txt", "one\ntwo\nthree\n" },
        { "/tmp/coretest/cat.txt",        "one\ntwo\nthree\n" },
        { "/tmp/coretest/unsorted.txt",   "e\nd\nc\nb\na\n" },
        { "/tmp/coretest/dups.txt",       "a\nb\nb\nc\nc\nc\n" },
        { "/tmp/coretest/nums.txt",       "10\n9\n007\n-3\n2.5\nabc\n" },
        { "/tmp/coretest/decimal.txt",    "2.50\n2.5\n" },
        { "/tmp/coretest/grepdir/top.txt",     "root one\nother\nroot two\n" },
        { "/tmp/coretest/grepdir/plain.txt",   "nothing here at all\n" },
        { "/tmp/coretest/grepdir/anchor.txt",  "^root one\n" },
        { "/tmp/coretest/grepdir/sub/mid.txt", "root mid\n" },
        { "/tmp/coretest/grepdir/sub/deep.txt","root deep\n" },
        { "/tmp/coretest/greedir/sub/only.txt", "needle\n" },
    };

    /* A directory for `grep -r` to descend, with one nested under it: the whole point of
     * `-r` is the part *below* the directory it was given, and a tree one deep does not
     * test that. */
    mkdir("/tmp/coretest/grepdir", 0755);
    mkdir("/tmp/coretest/grepdir/sub", 0755);
    mkdir("/tmp/coretest/greedir", 0755);
    mkdir("/tmp/coretest/greedir/sub", 0755);

    mkdir("/tmp/coretest/lsdir", 0755);
    mkdir("/tmp/coretest/coretestdir", 0755);

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        if (put_file(fixtures[i].path, fixtures[i].contents) != 0) {
            fprintf(stderr, "coretest: FAILED -- cannot write the fixture %s: %s\n",
                    fixtures[i].path, strerror(errno));
            failures++;
        }
    }

    static const check_t checks_table[] = {
        /* ---- echo --------------------------------------------------------------- */
        { "echo", { "hello", NULL }, NULL, "hello\n", NULL, 0,
          "echo prints its argument and a newline" },
        { "echo", { "two", "words", NULL }, NULL, "two words\n", NULL, 0,
          "echo separates several arguments with one space" },
        { "echo", { "-n", "no newline", NULL }, NULL, "no newline", NULL, 0,
          "echo -n does not print the trailing newline" },
        { "echo", { "a\\tb\\n", NULL }, NULL, "a\tb\n\n", NULL, 0,
          "echo expands backslash escapes, including a trailing one" },
        { "echo", { NULL }, NULL, "\n", NULL, 0,
          "echo with no arguments prints one empty line" },

        /* ---- cat ---------------------------------------------------------------- */
        { "cat", { "/tmp/coretest/cat.txt", NULL }, NULL, "one\ntwo\nthree\n", NULL, 0,
          "cat copies a file" },
        { "cat", { "/tmp/coretest/cat.txt", "/tmp/coretest/cat.txt", NULL }, NULL,
          "one\ntwo\nthree\none\ntwo\nthree\n", NULL, 0,
          "cat copies several files in order" },
        { "cat", { "/tmp/coretest/missing", NULL }, NULL, "", NULL, 1,
          "cat reports a missing file and fails" },
        { "cat", { "/tmp/coretest/cat.txt", "/tmp/coretest/missing", NULL }, NULL,
          "one\ntwo\nthree\n", NULL, 1,
          "cat reports and skips, and still exits non-zero" },

        /* ---- wc ----------------------------------------------------------------- */
        { "wc", { "-l", "/tmp/coretest/cat.txt", NULL }, NULL, "       3 /tmp/coretest/cat.txt\n",
          NULL, 0, "wc -l with a file name pads the count and prints the name" },
        /* The single bare count.  A command substitution reads this number, and padding it
         * made `lines=$(wc -l < f)` come out as `lines=       3`.  That was a real bug and
         * this is the check that keeps it fixed. */
        { "wc", { "-l", NULL }, "one\ntwo\nthree\n", "3\n", NULL, 0,
          "wc -l on standard input prints a bare count, unpadded" },
        { "wc", { "-w", NULL }, "one two three\n", "3\n", NULL, 0,
          "wc -w counts three words" },
        { "wc", { "-c", NULL }, "one\ntwo\nthree\n", "14\n", NULL, 0,
          "wc -c counts fourteen bytes" },

        /* ---- head and tail ------------------------------------------------------ */
        { "head", { "-n", "2", "/tmp/coretest/cat.txt", NULL }, NULL, "one\ntwo\n", NULL, 0,
          "head -n 2 prints the first two lines" },
        { "head", { "-n2", "/tmp/coretest/cat.txt", NULL }, NULL, "one\ntwo\n", NULL, 0,
          "head -n2 is the same as head -n 2" },
        /* No option *and* no file name, so this reads standard input.  The default is ten
         * lines and the file has three, which is the case that used to print nothing at all:
         * the start index was the next write's slot even when the ring was not full, so with
         * three lines and a window of ten it began at ring[3], which is empty. */
        { "head", { NULL }, "one\ntwo\nthree\n", "one\ntwo\nthree\n", NULL, 0,
          "head with no option prints ten lines, which is all of a three-line input" },
        { "head", { "-n", "1", "/tmp/coretest/cat.txt", NULL }, NULL, "one\n", NULL, 0,
          "head -n 1 prints one line" },

        { "tail", { "-n", "2", "/tmp/coretest/cat.txt", NULL }, NULL, "two\nthree\n", NULL, 0,
          "tail -n 2 prints the last two lines" },
        { "tail", { "-n", "1", "/tmp/coretest/cat.txt", NULL }, NULL, "three\n", NULL, 0,
          "tail -n 1 prints the last line" },
        { "tail", { "/tmp/coretest/cat.txt", NULL }, NULL, "one\ntwo\nthree\n", NULL, 0,
          "tail with no option prints ten lines, which is all of a three-line file" },
        { "tail", { "-n", "9", "/tmp/coretest/cat.txt", NULL }, NULL, "one\ntwo\nthree\n", NULL, 0,
          "tail of a file shorter than the window prints all of it" },

        /* ---- ls ----------------------------------------------------------------- */
        { "ls", { "/tmp/coretest/lsdir", NULL }, NULL, "cat.txt\nhead.txt\n", NULL, 0,
          "ls lists a directory, one name per line, sorted" },
        /* The whole name as it was given, not the base of it: `ls /etc/passwd` prints
         * `/etc/passwd`, and a script that reads the output of `ls` and then joins it to
         * something else needs the path it asked for back. */
        { "ls", { "/tmp/coretest/cat.txt", NULL }, NULL, "/tmp/coretest/cat.txt\n", NULL, 0,
          "ls of a file prints the name it was given" },
        { "ls", { "/tmp/coretest/nosuchdir", NULL }, NULL, "", NULL, 1,
          "ls of a missing directory fails" },

        /* ---- rm ----------------------------------------------------------------- */
        { "rm", { "/tmp/coretest/gone", NULL }, NULL, "", NULL, 1,
          "rm of a missing file fails" },
        { "rm", { "-f", "/tmp/coretest/gone", NULL }, NULL, "", NULL, 0,
          "rm -f of a missing file succeeds quietly" },
        { "rm", { "/tmp/coretest/coretestdir", NULL }, NULL, "", NULL, 1,
          "rm refuses a directory rather than recursing" },

        /* ---- tr ----------------------------------------------------------------- */
        { "tr", { "a-z", "A-Z", NULL }, "hi there\n", "HI THERE\n", NULL, 0,
          "tr a-z A-Z upper-cases" },
        { "tr", { "-d", "e", NULL }, "eee\neee\n", "\n\n", NULL, 0,
          "tr -d e deletes every e" },
        { "tr", { "-s", " ", NULL }, "a   b\n", "a b\n", NULL, 0,
          "tr -s squeezes runs of spaces" },
        { "tr", { "abc", "xy", NULL }, "abc\n", "xyy\n", NULL, 0,
          "tr with a short second set repeats its last byte" },

        /* ---- sort --------------------------------------------------------------- */
        { "sort", { "/tmp/coretest/unsorted.txt", NULL }, NULL, "a\nb\nc\nd\ne\n", NULL, 0,
          "sort puts lines in byte order" },
        { "sort", { "-r", "/tmp/coretest/unsorted.txt", NULL }, NULL, "e\nd\nc\nb\na\n", NULL, 0,
          "sort -r reverses" },
        { "sort", { "-u", "/tmp/coretest/dups.txt", NULL }, NULL, "a\nb\nc\n", NULL, 0,
          "sort -u drops adjacent duplicates" },
        { "sort", { "-n", "/tmp/coretest/nums.txt", NULL }, NULL,
          /* `abc` has no leading number, so it compares as zero and sorts first; `007` and
           * `7` would compare equal, and `9` comes before `10` where a byte sort would put
           * them the other way round. */
          "-3\nabc\n2.5\n007\n9\n10\n", NULL, 0,
          "sort -n compares numbers, and a line with no number sorts first" },
        { "sort", { "-n", "/tmp/coretest/decimal.txt", NULL }, NULL, "2.5\n2.50\n", NULL, 0,
          "sort -n treats 2.5 and 2.50 as equal and breaks the tie on the text" },

        /* ---- uniq --------------------------------------------------------------- */
        { "uniq", { "/tmp/coretest/dups.txt", NULL }, NULL, "a\nb\nc\n", NULL, 0,
          "uniq drops adjacent duplicates only" },
        { "uniq", { "-c", "/tmp/coretest/dups.txt", NULL }, NULL,
          "      1 a\n      2 b\n      3 c\n", NULL, 0,
          "uniq -c counts each run" },
        { "uniq", { "-d", "/tmp/coretest/dups.txt", NULL }, NULL, "b\nc\n", NULL, 0,
          "uniq -d prints only the repeated lines" },
        { "uniq", { "-u", "/tmp/coretest/dups.txt", NULL }, NULL, "a\n", NULL, 0,
          "uniq -u prints only the lines seen once" },
        /* ---- ls -l -------------------------------------------------------------- */
        { "ls", { "-l", "/tmp/coretest/lsdir/cat.txt", NULL }, NULL, NULL,
          /* `-rw??????` and not a whole mode string: a file made by `fopen("w")` is 0666
           * where there is no umask and 0644 where there is one, so pinning the nine
           * permission characters would make this a test of the umask.  What is checked is
           * the shape -- ten characters, a `-` for a regular file, the owner able to read
           * and write it -- and that is the same everywhere. */
          "-rw??????* 1 * * *?? /tmp/coretest/lsdir/cat.txt\n", 0,
          "ls -l on a file prints mode, links, owner, group, size and name" },
        { "ls", { "-l", "/tmp/coretest/lsdir", NULL }, NULL, NULL,
          "total 1\n-rw??????* 1 * * *?? cat.txt\n-rw??????* 1 * * *?? head.txt\n", 0,
          "ls -l on a directory prints a total and one line per entry, sorted" },
        { "ls", { "-ld", "/tmp/coretest/lsdir", NULL }, NULL, NULL, "drwxr-xr-x*", 0,
          "ls -ld describes the directory itself rather than its contents" },
        { "ls", { "-F", "/tmp/coretest/lsdir", NULL }, NULL, NULL, "cat.txt\nhead.txt\n", 0,
          "ls -F of a directory of files prints plain names" },

        /* ---- grep ---------------------------------------------------------------- */
        { "grep", { "root", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "root one\nroot two\n", 0, "grep prints the matching lines" },
        { "grep", { "-n", "root", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "1:root one\n3:root two\n", 0, "grep -n numbers the lines" },
        { "grep", { "-i", "ROOT", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "root one\nroot two\n", 0, "grep -i ignores case" },
        { "grep", { "-v", "root", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "other\n", 0, "grep -v prints the lines that do not match" },
        { "grep", { "-c", "root", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "2\n", 0, "grep -c counts the matching lines" },
        { "grep", { "-q", "root", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "", 0, "grep -q says nothing and exits 0 when there is a match" },
        { "grep", { "-q", "nothingatall", "/tmp/coretest/grepdir/top.txt", NULL }, NULL,
          NULL, "", 1, "grep -q exits 1 when there is not a match" },
        { "grep", { "nothingatall", "/tmp/coretest/grepdir/top.txt", NULL }, NULL, NULL,
          "", 1, "grep exits 1 when nothing matched, which is how a script tests" },
        { "grep", { "root", NULL }, "root on stdin\nother\n", NULL,
          "root on stdin\n", 0, "grep reads standard input when given no file" },
        /* One match, in one file, *below* the directory named.  The fixture is shaped that
         * way on purpose: `grep -r` walks names in whatever order `readdir()` returns them,
         * so a check over a tree with several matching files is a check on this filesystem's
         * mood.  One line cannot be in the wrong place. */
        { "grep", { "-r", "needle", "/tmp/coretest/greedir", NULL }, NULL, NULL,
          "/tmp/coretest/greedir/sub/only.txt:needle\n", 0,
          "grep -r descends below the directory it was given, and names the file" },
        /* With the name, because a recursive search always names its files -- one directory
         * was given, and the output is still the output of several searches. */
        { "grep", { "-r", "-c", "needle", "/tmp/coretest/greedir", NULL }, NULL, NULL,
          "*/only.txt:1\n", 0, "grep -rc gives a count per file, named" },
        /* Not checked over a tree with several matching files: `grep -r` walks names in
         * whatever order `readdir()` returns them, and this filesystem does not return them
         * in the same order the host's does.  A check that fails on one and not the other is
         * a check on readdir, not on grep.  The zero-count case is covered by the named-file
         * `-l` check above, where the order is the command line's. */
        { "grep", { "-l", "root", "/tmp/coretest/grepdir/top.txt",
                    "/tmp/coretest/grepdir/plain.txt", NULL }, NULL, NULL,
          "/tmp/coretest/grepdir/top.txt\n", 0,
          "grep -l lists the files that matched, and not the ones that did not" },
        /* The pattern is a fixed string, and this says so out loud: a regex engine would read
         * `^root` as an anchor and find nothing here. */
        { "grep", { "-F", "^root", "/tmp/coretest/grepdir/anchor.txt", NULL }, NULL, NULL,
          "^root one\n", 0,
          "grep treats the pattern as a fixed string, not a regular expression" },
        { "grep", { "root", "/tmp/coretest/grepdir/absent.txt", NULL }, NULL, NULL,
          "", 2, "grep reports a file it cannot open and exits 2" },
    };

    for (size_t i = 0; i < sizeof(checks_table) / sizeof(checks_table[0]); i++)
        run_check(&checks_table[i]);

    /* A file with no trailing newline, which both `sort` and `uniq` have to get right and
     * neither of the file-based checks above exercises -- every fixture ends with one. */
    put_file("/tmp/coretest/nonl.txt", "only");

    {
        static const check_t sort_nonl = {
            "sort", { "/tmp/coretest/nonl.txt", NULL }, NULL, "only\n", NULL, 0,
            "sort keeps a last line that has no terminator"
        };

        static const check_t uniq_nonl = {
            "uniq", { "/tmp/coretest/nonl.txt", NULL }, NULL, "only\n", NULL, 0,
            "uniq keeps a last line that has no terminator"
        };

        run_check(&sort_nonl);
        run_check(&uniq_nonl);
    }

    /* `rm` on a file that exists: this one is about doing something, and there is no output
     * to compare -- so it is checked by looking at the file afterwards. */
    checks++;

    if (put_file("/tmp/coretest/doomed", "x\n") != 0) {
        fail("rm", "cannot create the file to remove");
    } else {
        static const check_t rm_real = {
            "rm", { "/tmp/coretest/doomed", NULL }, NULL, "", NULL, 0,
            "rm removes a file and exits 0"
        };

        run_check(&rm_real);
        checks--;   /* run_check counted this one itself */

        FILE *f = fopen("/tmp/coretest/doomed", "r");

        if (f != NULL) {
            fclose(f);
            fail("rm", "left the file it was asked to remove");
        }
    }

    fprintf(stderr, "coretest: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}