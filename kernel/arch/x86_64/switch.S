/* minx - context switch between kernel tasks.
 *
 * The whole of it.  A kernel task's context is its callee-saved registers and
 * its stack pointer, so saving is six pushes and a store and restoring is a load
 * and six pops followed by a `ret` back into wherever the task was.
 */

.section .text

/* void task_switch(uint64_t *save_rsp, uint64_t new_rsp)
 *
 * Save the running task's callee-saved registers and stack pointer into
 * *save_rsp, then continue on the stack at new_rsp with that task's registers
 * restored.
 *
 * The System V callee-saved set is rbx, rbp, r12-r15.  Everything else belongs to
 * the caller and has already been spilled to the task's stack by whatever
 * interrupted it, which is why an interrupt handler can call this and still be
 * resumed exactly where it was: the stack this leaves behind is the handler's,
 * inside the interrupted task.
 *
 * The task being switched *to* must have a stack laid out so the pops below land
 * on its registers and the `ret` returns into its entry point.  task_bootstrap()
 * builds that.
 */
.global task_switch
.type task_switch, @function
task_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
.size task_switch, .-task_switch

/* void task_enter_first(uint64_t new_rsp)
 *
 * The first switch onto a task that has never run: there is nothing to save, so
 * this skips the store and goes straight to restoring.
 */
.global task_enter_first
.type task_enter_first, @function
task_enter_first:
    /* Interrupts on.  A task that has never run has no saved flags of its own, so
     * it would otherwise inherit whatever the processor happened to have -- and the
     * two places that switch to a never-run task are both reached from the timer
     * interrupt, where the interrupt gate has just cleared IF. */
    sti
    movq    %rdi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
.size task_enter_first, .-task_enter_first

/* Reached by the `ret` at the end of a first switch onto a task, so it runs with
 * the task's freshly built stack rather than a C frame.
 *
 * That `ret` has already consumed the return address, so %rsp points at the two
 * words the stack builder left above it: the entry point and its argument.  They
 * go into the argument registers and the real entry point is called.  Doing this
 * in assembly is what lets the entry point and the argument travel on a task's
 * stack at all -- the registers the restore puts back are all zero by
 * construction, because a task that has never run has nothing to put there.
 */
.global task_start_shim
.type task_start_shim, @function
task_start_shim:
    /* As in task_enter_first: a first entry has no saved flags, and a task whose
     * first act is to wait needs them on. */
    sti
    movq    (%rsp), %rdi         /* entry  */
    movq    8(%rsp), %rsi        /* arg    */
    addq    $16, %rsp
    jmp     task_start
.size task_start_shim, .-task_start_shim

.section .note.GNU-stack,"",@progbits