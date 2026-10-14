/* minx - the C runtime start-up.
 *
 * Three jobs, in this order, and the order is the whole of it:
 *
 *   1. Work out the stack the kernel built, so `main()` gets an `argc`/`argv` that mean
 *      something.
 *   2. Initialise the libc -- the streams, the environment -- so that the first `printf()`
 *      in `main()` works and the first `getenv()` finds `PATH`.
 *   3. Call `main()`, then `exit()` with what it returned.
 *
 * Step 3 is the one with a decision in it.  A program that returns from `main()` must reach
 * `exit()`, not fall off the end of the entry stub: the difference is whether the atexit
 * handlers run and whether the stdio buffers are flushed, and a program whose last line of
 * output is written by a buffer that was never flushed has lost exactly the line a user
 * would have read first.
 */
#include <stdbool.h>
#include <internal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* `main()` is declared here rather than included, because a program's own `main()` is the
 * one being called and <stdlib.h> has no declaration for it.  The prototype is what makes
 * the compiler check the program's definition, which is the point of having it. */
extern int main(int argc, char **argv, char **envp);

/* Both of these are in stdlib.c, which is where the environment lives.  They are called
 * rather than assigned to directly because `environ` is that file's global and a second
 * definition of it is a link error -- so the crt asks the module that owns it to change
 * it. */
void __libc_set_startup_env(char **envp);
void __libc_adopt_env(char **envp);

/* The environment a program starts with when the loader gave it none.
 *
 * `PATH`, `HOME` and `PWD`, because a program cannot do without any of them: `PATH`
 * because `execvp()` with no PATH searches nothing and a shell's first `exec` then fails;
 * `HOME` because a program that cannot find a home directory either refuses to start or,
 * worse, writes to `/`; and `PWD` because a shell's idea of where it is has to come from
 * somewhere and the kernel's working directory is not visible until a program asks.
 *
 * These are strings in this file, so they live in the image and outlive everything -- which
 * is what makes them safe to hand to `setenv()`, whose strings are never freed. */
static char env_path[] = "PATH=/bin:/usr/bin";
static char env_home[] = "HOME=/";
static char env_pwd[]  = "PWD=/";

static char *startup_env[] = { env_path, env_home, env_pwd, NULL };

/* `environ` itself belongs to stdlib.c, which is where `setenv()` also writes it -- two
 * definitions of a global is a link error, and the right answer is that the one that owns
 * the environment's *table* owns the pointer to it too. */
void __libc_set_startup_env(char **envp) {
    __libc_adopt_env(envp != NULL ? envp : startup_env);
}

/* Set `environ` up before `main()`.
 *
 * `stdin`, `stdout` and `stderr` need their buffering decided before the first `printf`,
 * and that decision depends on whether they are a terminal -- which needs an `ioctl`, which
 * needs no allocation.  Both happen here so that `main()` can be written as if none of it
 * exists. */
static void libc_startup(void) {
    __stdio_init();
}

int __libc_start_main(long *stack) {
    /* The stack the loader built is the SysV shape: at the stack pointer, `argc`; above it
     * `argv[0]`, the NULL that ends argv, then envp, then the auxiliary vector.  So the
     * three arguments are three different words of it, and reading only the first is the
     * mistake that makes a program report an argc of 140737488350976 -- which is a stack
     * address, and which a program that handles argc == 0 gracefully will happily carry on
     * with.
     *
     * argv[0] is set when the kernel started the program without one, which is what happens
     * for a program run by the kernel itself rather than by a shell.  A program that reads
     * argv[0] to find its own name gets something rather than NULL. */
    long    argc = stack[0];
    char   **argv = (char **)&stack[1];

    /* Whether `argv` was replaced below.  It has to be known *before* the environment is
     * looked for, and remembering it is the whole of the fix. */
    bool substituted = false;

    if (argc < 1) {
        static char *fallback[] = { (char *)"minx", NULL };

        argv = fallback;
        argc = 1;
        substituted = true;
    }

    if (argv[0] == NULL) {
        static char *fallback0[] = { (char *)"minx", NULL };

        argv = fallback0;
        argc = 1;
        substituted = true;
    }

    /* The loader's environment, found from the loader's *stack*.
     *
     * It used to be found from `argv`, which is wrong the moment `argv` is one of the two
     * two-element arrays above: `argv + argc + 1` is then **one past the end of that array**,
     * and the word read there is whatever the linker happened to place after it in `.bss`.
     * Zero there gives the libc's own environment, which is right.  Anything else is adopted
     * as an environment -- a pointer to a number, read as a list of strings -- and every
     * `getenv()` in the program then returns NULL for everything.
     *
     * **That is the intermittent "PATH is set before main()" failure**: it depends on what
     * follows a `static` array in `.bss`, which changes with every edit to this file, and it
     * has been in and out of `make test` for a long time without ever being a real
     * difference between the machines it failed on and the machines it passed.
     *
     * A substituted `argv` is not the loader's, so there is no loader's environment to find
     * from it, and `NULL` is the honest answer: the libc's own three variables are then used,
     * which is what a program the kernel started should see anyway. */
    char **envp = substituted ? NULL : argv + argc + 1;

    /* The loader's envp, if it supplied one, replaces the libc's.  A program started by a
     * shell has the shell's environment, which is what it should see; a program started by
     * the kernel has none, and gets the three variables above. */
    /* Both tests, and the second one is not redundant.  `envp` can be a perfectly good
     * pointer to an array whose *first element* is NULL -- that is exactly what the loader
     * writes for a program it starts with no environment -- and `__libc_adopt_env()` reads
     * those two as different things: a NULL pointer means "use the libc's defaults", an
     * empty array means "this program has no variables".
     *
     * Passing the empty array through drops every `getenv()` in the program to NULL, which
     * is what happened here: every check in `coretest` failed with `execvp()` reporting it
     * had tried to run `.`, because PATH had gone.  The loader *does* write the `envp[0]`
     * slot -- `sp+24`, and it is NULL for exactly this case -- so the pointer is good and
     * what it holds is not. */
    __libc_set_startup_env(envp != NULL && envp[0] != NULL ? envp : NULL);

    libc_startup();

    int rc = main((int)argc, argv, environ);

    /* `exit()` rather than `_exit()`, and that is the point of this function existing: the
     * stdio buffers are flushed and the atexit handlers run, so a program that printed
     * something during `main()` and returned has actually printed it. */
    exit(rc);

    /* `exit()` does not return.  It is declared `_Noreturn` in the C standard, which this
     * build's <stdlib.h> does not have, so the compiler cannot know that -- and a function
     * that might return has to end somewhere.  Returning `rc` rather than looping means a
     * hypothetical bug surfaces as "main's return value", which is diagnosable, instead of
     * as a hang. */
    return rc;
}