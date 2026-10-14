/* minx - `shtest`: does the shell do what it says it does?
 *
 * The shell is the one program in this tree with too much surface to test by running it and
 * looking.  So it is tested the way every Unix tests a shell: a script, an expected output,
 * and a comparison.
 *
 * What is compared is *output*, not internals, and that is the whole design.  A test that
 * checked whether `exec_cmd` called `fork` would pass with a shell that forked at the wrong
 * moment.  A test that runs `for i in 1 2 3; do echo $i; done` and checks the three lines
 * cannot.
 *
 * The script is written to the filesystem from here rather than shipped in the initrd, so
 * that this program and the shell it tests have no build-time dependency on each other: the
 * script is part of the test, and the test is part of the tree.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks;
static int failures;

/* The script.
 *
 * Every line here is one thing, and the expected output is a list of the same lines.  A test
 * that bundles several behaviours into one command reports one answer, and the answer is
 * usually "something failed" with no indication of what -- which is the failure mode of every
 * shell test suite that writes `echo 1; echo 2; echo 3` and checks for "2".
 */
static const char *script =
    "# shtest: the script under test.\n"
    "echo one\n"
    "echo \"two three\"\n"
    "echo 'four  five'\n"
    /* An empty line, and the reason it is `echo \"\"` rather than a blank line: a blank line
     * in a script is not an `echo`, and nothing produces output for it.  The expected list
     * has an empty string here because the line was *meant* to print one, and a test whose
     * script says something different from what it checks is a test that fails for the right
     * reason in the wrong way -- every later line then reports as out of order, because the
     * comparison walks them in step.  Which is what happened: one missing blank shifted all
     * fifty-one comparisons after it.
     *
     * So the script says what the list means.  A blank line is still in the script further
     * down, where it belongs -- this shell has to skip one, and there is no check that it
     * does. */
    "echo \"\"\n"
    "x=42\n"
    "echo \"x=$x\"\n"
    "echo \"len=${#x}\"\n"
    "echo \"default=${nothere:-fallback}\"\n"
    "echo \"empty=${empty:-fallback}\"\n"
    "echo \"set=${x:+present}\"\n"
    "echo \"sub=$(echo inner)\"\n"
    "echo \"nested=$(echo $(echo deep))\"\n"
    "echo \"arith=$((2 + 3 * 4))\"\n"
    "n=0\n"
    "n=$((n + 1))\n"
    "echo \"n=$n\"\n"
    "\n"
    "echo \"quoted=$*\"\n"
    "set -- p q\n"
    "echo \"at=$1$2\"\n"
    "echo \"star=$*\"\n"
    "echo \"atq=$@\"\n"
    "\n"
    "for i in 1 2 3; do echo \"i=$i\"; done\n"
    "for w in a b; do echo \"w=$w\"; done\n"
    "\n"
    "if true; then echo then; else echo else; fi\n"
    "if false; then echo bad; elif true; then echo elif; fi\n"
    "if false; then echo bad; else echo els; fi\n"
    "\n"
    "n=0\n"
    "while [ $n -lt 3 ]; do n=$((n + 1)); echo \"w=$n\"; done\n"
    "n=0\n"
    "until [ $n -ge 2 ]; do n=$((n + 1)); done\n"
    "echo \"until=$n\"\n"
    "\n"
    "case abc in a*) echo case-star;; b*) echo case-b;; *) echo case-none;; esac\n"
    "case xyz in a*) echo case-star;; *) echo case-default;; esac\n"
    "case one in one|two) echo case-alt;; esac\n"
    "\n"
    "echo \"and=$(true && echo yes)\"\n"
    "echo \"or=$(false || echo also)\"\n"
    "false && echo unreached\n"
    "echo \"status=$?\"\n"
    "true || echo unreached\n"
    "\n"
    "( echo subshell )\n"
    "{ echo group; }\n"
    "\n"
    "greet() { echo \"hello $1\"; }\n"
    "greet world\n"
    "greet again\n"
    "\n"
    "cd /tmp || echo \"cd failed\"\n"
    "echo \"pwd=$(pwd)\"\n"
    "cd / || echo \"cd failed\"\n"
    "\n"
    "echo 'to stdout'\n"
    "echo 'to stderr' >&2\n"
    /* The line above is the one `>&2` test that cannot be checked here: it goes to the
     * console, and this program's output is a file, so there is nothing in it to compare
     * against.  The pair below is that test *and* the `2>&1` test, in one construct, and it
     * can be checked: the group's own descriptor 2 is sent to its descriptor 1, so both
     * lines land in the file, and `echo 'captured err' >&2` reaching that file is only
     * possible if `>&2` really wrote to descriptor 2 rather than to whatever was in it.
     *
     * Which is the stronger claim: the old line only showed that `>&2` did not appear in the
     * output, which is also what a redirection that was silently ignored would look like. */
    "{ echo 'captured out'; echo 'captured err' >&2; } 2>&1\n"
    "echo 'redirected' > /tmp/shtest-out\n"
    "echo \"file=$(cat /tmp/shtest-out)\"\n"
    "echo 'appended' >> /tmp/shtest-out\n"
    "echo \"lines=$(wc -l < /tmp/shtest-out)\"\n"
    "echo \"tail=$(tail -n 1 /tmp/shtest-out)\"\n"
    "\n"
    "echo \"glob=$(ls /tmp/shtest-* | head -n 1)\"\n"
    "rm -f /tmp/shtest-out\n"
    "\n"
    "true | cat\n"
    "echo piped\n"
    "echo x >&2 2>/dev/null\n"
    "echo \"after-redir=$?\"\n"
    "\n"
    "sh -c 'echo from -c'\n"
    "echo \"cstatus=$?\"\n"
    "sh -c 'exit 3'\n"
    "echo \"exit=$?\"\n"
    "\n"
    "echo final\n";

/* What the script must print, in order.
 *
 * This is the specification of the shell for this test.  Every line here is a promise about
 * behaviour rather than about implementation: `x=$x` keeps one argument, `echo ''` prints an
 * empty line, `2>&2 2>/dev/null` redirects the error and nothing else.  A change in the
 * script without a change here means the test no longer tests what it tested; a change here
 * without one in the script means the test has found a difference. */
static const char *expected[] = {
    "one",
    "two three",
    "four  five",
    "",
    "x=42",
    "len=2",
    "default=fallback",
    "empty=fallback",
    "set=present",
    "sub=inner",
    "nested=deep",
    "arith=14",
    "n=1",
    "quoted=",
    "at=pq",
    "star=p q",
    "atq=p q",
    "i=1",
    "i=2",
    "i=3",
    "w=a",
    "w=b",
    "then",
    "elif",
    "els",
    "w=1",
    "w=2",
    "w=3",
    "until=2",
    "case-star",
    "case-default",
    "case-alt",
    "and=yes",
    "or=also",
    "status=1",
    "subshell",
    "group",
    "hello world",
    "hello again",
    "pwd=/tmp",
    "to stdout",
    /* `echo 'to stderr' >&2` is in the script and deliberately *not* here: it goes to the
     * console, and this list is the contents of the file the script's output was collected
     * into.  Expecting it anyway is how the whole list came to be shifted by one -- every
     * comparison after it reported as out of order, because the comparison walks the two
     * lists in step and a line that is never in one of them shifts all of them. */
    "captured out",
    "captured err",
    "file=redirected",
    "lines=2",
    "tail=appended",
    "glob=/tmp/shtest-out",
    "piped",
    "after-redir=0",
    "from -c",
    "cstatus=0",
    "exit=3",
    "final",
};

static void check(const char *what, int ok) {
    checks++;

    if (ok) {
        printf("shtest: ok -- %s\n", what);
        return;
    }

    printf("shtest: FAILED -- %s\n", what);
    failures++;
}

/* Write the script and run it with its output on a pipe.
 *
 * The pipe is drained *before* the child is waited for, and that ordering is not an
 * optimisation: a script that prints more than a pipe buffer blocks on the write, and a test
 * that waited first would wait for ever.  `cat /proc/meminfo` is more than 4 KiB, so this is
 * a case that happens rather than one that might. */
/* Run the script with its output redirected to a file, and read the file back.
 *
 * A file rather than a pipe.  It is the same test either way -- what is compared is the
 * script's output, not the mechanism it came through -- and a file can be read after the
 * child has finished, which means the harness has one less moving part: no draining a pipe
 * that a child is writing to, no second failure mode when the two ends of it disagree, and
 * no interleaving of the child's output with the parent's diagnostics in the serial log,
 * which is what made the first version of this harness unreadable when it went wrong.
 *
 * The output is read through `fread()` into a static buffer rather than with a `read()` loop
 * into a `malloc()`ed one.  `shtest` has nothing to gain from growing its own buffer -- the
 * script's output is a few kilobytes and the bound is checked -- and a loop that reallocs
 * while a child is writing into the same pipe is a loop with two failure modes instead of
 * one. */
#define SHT_OUT_MAX 32768
static char out_buf[SHT_OUT_MAX];

/* Where this program's own diagnostics go.
 *
 * A file, appended to, one line at a time -- and the reason is that the console cannot
 * carry them.  When two copies of this program run at once (which is the thing being
 * chased) every line they print is interleaved with the other copy's *byte by byte*, so
 * the log shows a sentence that belongs to no program and answers no question.  A file
 * opened with `O_APPEND` and written with one `write()` per line gives each line to one
 * process, so the file says which copy took which branch.
 *
 * The console still gets the lines, because a failure a reader has to go looking for in a
 * ramfs is a worse first impression than one that is already on screen.
 *
 * The leading dot is not decoration.  The script under test runs `ls /tmp/shtest-*`, and
 * `/tmp/shtest-child`, `/tmp/shtest-out`, `/tmp/shtest.sh` and `/tmp/shtest-trace` all
 * match that glob -- so a diagnostic file named without the dot became the answer to the
 * script's own question about which files a redirection had created, and the test failed
 * for having reported something.  A name `*` cannot match is a name that cannot interfere. */
#define SHT_TRACE "/tmp/.shtest-trace"

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void trace(const char *fmt, ...) {
    char    line[192];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;

    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;

    line[n++] = '\n';

    int fd = open(SHT_TRACE, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd >= 0) {
        (void)!write(fd, line, (size_t)n);
        close(fd);
    }

    (void)!write(2, line, (size_t)n);
}

static char *run_script(int *status_out) {
    int fd = open("/tmp/shtest.sh", O_WRONLY | O_CREAT | O_TRUNC, 0755);

    if (fd < 0) {
        printf("shtest: could not create /tmp/shtest.sh: %s\n", strerror(errno));
        return NULL;
    }

    size_t len = strlen(script);
    size_t at = 0;
    ssize_t n = 0;

    while (at < len) {
        n = write(fd, script + at, len - at);

        if (n <= 0)
            break;

        at += (size_t)n;
    }

    close(fd);

    /* A script that was not written whole is not a script, and testing it anyway produces a
     * failure report about the *shell* when the fault is here: the shell runs an empty file,
     * prints nothing, exits 0, and every expected line is missing.  Say so instead. */
    if (at != len) {
        printf("shtest: wrote %zu of %zu bytes of the script: %s\n", at, len,
               n <= 0 ? strerror(errno) : "a short write");
        return NULL;
    }

    unlink("/tmp/shtest.out");

    /* Opened here rather than in the child, and that is not tidiness: the child does
     * nothing but `dup2` this descriptor and `execve`, so the one thing that can go wrong
     * in the child (opening a file) has already happened, in the parent, where its error can
     * be reported with a message instead of an exit status nobody reads. */
    int out = open("/tmp/shtest.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (out < 0) {
        printf("shtest: could not create /tmp/shtest.out: %s\n", strerror(errno));
        return NULL;
    }

    pid_t pid = fork();

    if (pid < 0) {
        printf("shtest: fork: %s\n", strerror(errno));
        return NULL;
    }

    /* One line per copy, in a file, before anything else can go wrong. */
    trace("shtest: pid %ld: fork returned %ld", (long)getpid(), (long)pid);

    if (pid == 0) {
        /* A marker, and on a file rather than on the console.
         *
         * Two copies of this program have been seen running at once, each convinced it
         * was the parent, and the only evidence was two messages interleaved character by
         * character in the serial log: unreadable, and unable to say which branch each
         * copy took.  A marker *in a file* survives that, and the self test already dumps
         * the files this program leaves behind when it fails, so the marker's contents end
         * up in the failure report without anything having to read it at the time.
         *
         * The console gets a copy too, because a file that exists but was never written is
         * a different answer from a file that was never created, and only the pair says
         * which. */
        {
            static const char here[] = "shtest: the child branch reached, about to exec\n";
            int            fd = open("/tmp/.shtest-child", O_WRONLY | O_CREAT | O_TRUNC,
                                      0644);

            if (fd >= 0) {
                (void)!write(fd, here, sizeof(here) - 1);
                close(fd);
            }

            trace("shtest: pid %ld: the child branch reached, about to exec",
                  (long)getpid());
        }

        /* The child's standard output becomes the file the parent opened. */
        if (dup2(out, 1) < 0)
            _exit(126);

        char *argv[3];
        char *env[64];
        int   n = 0;

        argv[0] = (char *)"sh";
        argv[1] = (char *)"/tmp/shtest.sh";
        argv[2] = NULL;

        for (int i = 0; environ != NULL && environ[i] != NULL && n < 63; i++)
            env[n++] = environ[i];

        env[n] = NULL;

        execve("/bin/sh", argv, env);

        /* A raw `write()` and not a `printf()`, because `_exit()` does not flush a stdio
         * buffer -- so a `fprintf(stderr, ...)` is exactly the kind of diagnostic that is
         * lost in the one case it was written for.  Descriptor 2 is still the console: only
         * 1 was replaced. */
        fprintf(stderr, "shtest: /bin/sh would not start: %s\n", strerror(errno));
        _exit(127);
    }

    int    st = 0;
    pid_t  reaped;

    while ((reaped = waitpid(pid, &st, 0)) < 0 && errno == EINTR)
        ;

    trace("shtest: pid %ld: waitpid returned %ld for the pid %ld asked about, "
          "status word %ld", (long)getpid(), (long)reaped, (long)pid, (long)st);

    /* A `waitpid` that failed is not a script that passed.
     *
     * It used to be folded into the status: a `waitpid` returning -1 left `st` at zero,
     * `WIFEXITED(0)` is true and `WEXITSTATUS(0)` is zero, so a shell that never ran at all
     * was reported as a shell that succeeded, and the test compared an empty file against
     * its list of expected lines and said so -- which is a real failure, but one that reads
     * as "the script printed the wrong thing" rather than "there was no script". */
    if (reaped < 0) {
        trace("shtest: pid %ld: waitpid: %s", (long)getpid(), strerror(errno));
        printf("shtest: waitpid: %s\n", strerror(errno));
        close(out);
        return NULL;
    }

    if (reaped != pid) {
        printf("shtest: waitpid returned %ld, not the child %ld\n", (long)reaped,
               (long)pid);
        close(out);
        return NULL;
    }

    *status_out = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);

    /* Read the whole file.  `fread()` rather than a `read()` loop: one call, one failure
     * mode, and the buffer is a fixed size the linker can account for. */
    FILE *f = fopen("/tmp/shtest.out", "r");

    if (f == NULL) {
        printf("shtest: the script produced no output: %s\n", strerror(errno));
        out_buf[0] = 0;
        return out_buf;
    }

    size_t got = fread(out_buf, 1, sizeof(out_buf) - 1, f);

    fclose(f);
    out_buf[got] = 0;

    if (got >= sizeof(out_buf) - 1)
        printf("shtest: the script produced more than %zu bytes and was truncated\n",
               sizeof(out_buf) - 1);

    return out_buf;
}

/* Split the output into lines, keeping empty ones -- an empty line *is* output, and dropping
 * them would make `echo ""` invisible to the test.  In place: each line is terminated where
 * its newline was, and the pointers are into the one buffer. */
static char **split_lines(char *text, int *count) {
    /* A static array rather than an allocation: this is called once, the result is used
     * before anything else can call it, and an allocation would mean a size the caller has
     * to agree with. */
    static char *lines[512];
    int         n = 0;

    char *p = text;

    while (*p != 0 && n < 512) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');

        if (nl == NULL)
            break;

        *nl = 0;
        p = nl + 1;
    }

    *count = n;
    return lines;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    /* Unbuffered, because a test that dies leaves nothing behind otherwise and the failure
     * report is the whole point of a test. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Which process is talking.
     *
     * This program forks and `exec`s, and a pid in the first line settles in one run what a
     * page of reasoning about `fork` and `exec` does not settle in ten -- in particular
     * whether a line came from the parent or from a copy of it. */
    printf("shtest: pid %ld\n", (long)getpid());

    int    status = -1;
    char  *out = run_script(&status);

    if (out == NULL) {
        printf("shtest: the script could not be run\n");
        return 1;
    }

    int    got = 0;
    char **lines = split_lines(out, &got);
    int    want = (int)(sizeof(expected) / sizeof(expected[0]));

    check("the script exits 0", status == 0);

    /* Line by line, and each one named by what it is rather than by its index -- a report
     * that says "line 17 differs" sends the reader to the script; one that says
     * "`case-default` did not appear" does not. */
    int matched = 0;

    for (int i = 0; i < want && i < got; i++) {
        if (strcmp(lines[i], expected[i]) == 0) {
            matched++;
            continue;
        }

        printf("shtest: line %d: wanted \"%s\", got \"%s\"\n", i + 1, expected[i],
               lines[i]);
        failures++;
    }

    checks += want;

    if (got > want) {
        for (int i = want; i < got; i++)
            printf("shtest: unexpected extra line: \"%s\"\n", lines[i]);

        failures += got - want;
    }

    printf("shtest: %d of %d lines matched\n", matched, want);

    check("every expected line appeared, in order", matched == want);
    check("the script printed no lines beyond the expected ones", got == want);

    /* Only on success.  These three files are the whole of what is known about a failure,
     * and the self test dumps them when this program reports one -- so removing them here,
     * unconditionally, is removing the evidence before anybody has read it.  A run that
     * passed has nothing to show and does not need them; a run that failed leaves them for
     * the report. */
    if (failures == 0) {
        unlink("/tmp/shtest.sh");
        unlink("/tmp/shtest.out");
        unlink("/tmp/.shtest-child");
        unlink(SHT_TRACE);
    }

    if (failures != 0) {
        printf("shtest: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }

    printf("shtest: ok -- %d checks\n", checks);
    return 0;
}
