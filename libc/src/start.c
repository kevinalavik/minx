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

    if (argc < 1) {
        static char *fallback[] = { (char *)"minx", NULL };

        argv = fallback;
        argc = 1;
    }

    if (argv[0] == NULL) {
        static char *fallback0[] = { (char *)"minx", NULL };

        argv = fallback0;
        argc = 1;
    }

    char **envp = argv + argc + 1;

    /* The loader's envp, if it supplied one, replaces the libc's.  A program started by a
     * shell has the shell's environment, which is what it should see; a program started by
     * the kernel has none, and gets the three variables above. */
    __libc_set_startup_env(*envp != NULL ? envp : NULL);

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