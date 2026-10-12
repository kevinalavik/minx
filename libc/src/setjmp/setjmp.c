/* minx - the signal mask half of `sigsetjmp` and `siglongjmp`.
 *
 * The other half -- capturing and restoring the registers -- is assembly, in asm.S beside
 * this file, and the reason it has to be is written there.
 *
 * What is here is the part that is a syscall, and which therefore cannot be: the blocked
 * signal set is the kernel's, and only a call into it can read or change one.
 *
 * Both halves are careful about *when* they run, and the order is forced rather than
 * chosen:
 *
 *   - `__sigjmp_save_mask()` is called by `sigsetjmp()` *after* the registers are captured.
 *     It cannot be before, because a call would change `%rsp` and the capture reads it.
 *
 *   - `__sigjmp_restore_mask()` is called by `siglongjmp()` *before* the registers are
 *     restored.  It cannot be after, because the restore jumps away and never comes back.
 */
#include <setjmp.h>
#include <signal.h>

/* The saved set lives in the ninth word of a `sigjmp_buf`: after rbx, rbp, r12, r13, r14,
 * r15, rsp and rip -- the eight the assembly writes.  `JB_MASK` is named here rather than
 * written as a literal because the number is a contract with asm.S and a reader should not
 * have to count to check it. */
#define JB_MASK   8

/* Save the blocked set, or clear the slot when it is not wanted.
 *
 * `SIG_SETMASK` with an empty set *asks* for the current mask rather than setting one, which
 * is exactly the question being asked here and costs one syscall.  Doing it only when
 * `save_mask` is set is the whole difference between `setjmp()` and `sigsetjmp()`: the
 * first must not touch the kernel, because a program using it has no reason to.
 *
 * The slot is cleared rather than left alone when the mask is not wanted, so that
 * `__sigjmp_restore_mask()` can tell the two cases apart -- a cleared slot means "this was
 * a plain `setjmp`", and restoring an empty set over a program that never asked would
 * unblock signals the program had blocked for its own reasons. */
void __sigjmp_save_mask(sigjmp_buf env, int save_mask) {
    if (save_mask) {
        sigset_t none;

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, &env[JB_MASK]);
    } else {
        env[JB_MASK] = 0;
    }
}

/* Put a saved set back.
 *
 * One syscall, and the null `old` because nothing here wants to know what the set was
 * before.  A `siglongjmp()` that did not do this would arrive with every signal the handler
 * had blocked now unblocked, and the signal that interrupted it would be delivered again.
 */
void __sigjmp_restore_mask(sigjmp_buf env) {
    if (env[JB_MASK] != 0)
        sigprocmask(SIG_SETMASK, &env[JB_MASK], NULL);
}

/* Silence a warning about the index macro: `JB_RIP` is documented here because the layout
 * is a contract with asm.S, and naming the two slots next to each other is what keeps the
 * next reader from guessing. */
