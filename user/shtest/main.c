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
    "\n"
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
    "to stderr",
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

static char *run_script(int *status_out) {
    int fd = open("/tmp/shtest.sh", O_WRONLY | O_CREAT | O_TRUNC, 0755);

    if (fd < 0) {
        printf("shtest: could not create /tmp/shtest.sh: %s\n", strerror(errno));
        return NULL;
    }

    size_t len = strlen(script);
    size_t at = 0;

    while (at < len) {
        ssize_t n = write(fd, script + at, len - at);

        if (n <= 0)
            break;

        at += (size_t)n;
    }

    close(fd);

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

    if (pid == 0) {
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
        _exit(127);
    }

    int st = 0;

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;

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

    unlink("/tmp/shtest.sh");
    unlink("/tmp/shtest.out");

    if (failures != 0) {
        printf("shtest: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }

    printf("shtest: ok -- %d checks\n", checks);
    return 0;
}
