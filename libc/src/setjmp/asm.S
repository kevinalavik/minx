/* minx - `setjmp`, `longjmp`, and their signal-aware forms.
 *
 * All four are assembly, and the reason is specific rather than general.
 *
 * The obvious way to write `setjmp()` is to capture the return address from "one frame out"
 * -- to call a helper that reads `8(%rsp)` -- and that is wrong the moment the compiler
 * pushes anything before the call.  `setjmp()` here was compiled with a `push %rbx`, so
 * `8(%rsp)` was the *saved rbx*, not a return address; `libctest` jumped into `.rodata` and
 * took a general protection fault on an instruction that was a string.
 *
 * So the capture is the entry point itself: no pushes, no calls, and `(%rsp)` is the return
 * address because nothing has touched it.  That is the whole reason for the assembly.
 *
 * The saved set, in order, is: rbx, rbp, r12, r13, r14, r15, rsp, rip.  Everything else is
 * dead across a `longjmp` by definition -- that is the contract, and a program that reads a
 * local across one is reading a register that held something else a microsecond ago.
 *
 * The `rsp` in that set is *not* `%rsp` as it stands here.  It is `%rsp + 8`, and the eight
 * bytes are the return address this function is about to `ret` through: a caller does not
 * carry on with the stack it had at the `call`, it carries on with the stack it had after
 * the `call` returned, and a `longjmp` that restores the call-time value leaves every
 * caller one word too high.  A stack frame is addressed relative to `%rsp`, so "one word too
 * high" does not crash -- every load and store lands in the right frame, eight bytes off --
 * and the program keeps running quite happily until the epilogue's `ret` picks up a return
 * address that is not one.  `libctest` printed every one of its results correctly and then
 * jumped to address 0 on the way out of `main`, which is what that looks like.
 *
 * The signal mask, for the two `sig` forms, is saved by a C helper called *after* the
 * capture and restored by a C helper called *before* the jump.  Both orders are forced:
 * the capture cannot be followed by anything that changes `%rsp`, and the jump cannot be
 * preceded by anything that must still happen.
 */
.text

/* int setjmp(unsigned long *env)
 *
 * `env` is rdi.  No prologue, no calls: at the first instruction `(%rsp)` is the address
 * `setjmp` will return to, which is the whole of what this function has to find out. */
.globl setjmp
.type  setjmp, @function
setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq     8(%rsp), %rax      /* the stack the caller will have, not this one */
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax          /* the value `setjmp` itself returns */
    ret
.size setjmp, .-setjmp

/* int sigsetjmp(unsigned long *env, int save_mask)
 *
 * `env` is rdi and `save_mask` is esi.  The capture is the same eight stores and nothing
 * else; the mask is saved afterwards by a call, which is safe *because* the capture is
 * already done -- a `call` pushes a return address, and by then nobody is reading `(%rsp)`.
 *
 * That helper preserves the callee-saved registers, because it is C and the ABI says so,
 * which is what makes it safe to call at this point at all. */
.globl sigsetjmp
.type  sigsetjmp, @function
sigsetjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq     8(%rsp), %rax      /* the stack the caller will have, not this one */
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    movq    %rdi, %rdi
    movl    %esi, %esi
    call    __sigjmp_save_mask  /* sets env[8], or clears it when save_mask is 0 */
    xorl    %eax, %eax
    ret
.size sigsetjmp, .-sigsetjmp

/* void longjmp(unsigned long *env, int value)
 *
 * The value 0 becomes 1, because an arrival that looked like the `setjmp` that saved the
 * environment would be indistinguishable from it -- and a program whose error handling is
 * "if (setjmp(...))" would take the success path.
 *
 * The jump is a tail `jmp` rather than a `call`: nothing comes back, and pushing a return
 * address that is about to be thrown away is noise.  `__longjmp_restore` overwrites `%rsp`
 * from the environment before it jumps, so whatever is on the stack is irrelevant. */
.globl longjmp
.type  longjmp, @function
longjmp:
    testl   %esi, %esi
    jnz     1f
    movl    $1, %esi
1:  movl    %esi, %esi          /* __longjmp_restore takes the value in esi */
    jmp     __longjmp_restore
.size longjmp, .-longjmp

/* void siglongjmp(unsigned long *env, int value)
 *
 * The mask goes back *first*, by a call, because the register restore and the jump are the
 * last thing that happens: anything after them in this function does not happen at all.
 *
 * A `longjmp()` that arrives with a different blocked set than the one it left is not a
 * `longjmp()`.  It is a jump that has quietly re-enabled every signal the handler had
 * blocked, and the signal that interrupted the handler is delivered again -- and again, for
 * ever. */
.globl siglongjmp
.type  siglongjmp, @function
siglongjmp:
    movq    %rdi, %rdi
    call    __sigjmp_restore_mask
    testl   %esi, %esi
    jnz     1f
    movl    $1, %esi
1:  movl    %esi, %esi
    jmp     __longjmp_restore
.size siglongjmp, .-siglongjmp

/* void __longjmp_restore(unsigned long *env, int value)
 *
 * Restore everything, put the value where a return would leave it, and jump.  There is no
 * return: the frame this is running in is abandoned on the way out, which is the point.
 *
 * `%rax` is set *before* `%rsp` and the jump, and it is the one register here that is not
 * restored from the buffer -- because it is the one register the whole thing exists to
 * produce.  `setjmp` returned 0 through it; `longjmp` returns whatever it was given.
 *
 * The return address is read out of the buffer *first*, before `%rdi` is overwritten with
 * the value: `%rdi` names the buffer, and loading it with the value before reading the
 * buffer would read the wrong eight bytes. */
.globl __longjmp_restore
.type  __longjmp_restore, @function
__longjmp_restore:
    movq    56(%rdi), %rcx      /* where to return to */
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movl    %esi, %eax          /* the value, in the return register */
    jmpq    *%rcx
.size __longjmp_restore, .-__longjmp_restore

.section .note.GNU-stack,"",@progbits