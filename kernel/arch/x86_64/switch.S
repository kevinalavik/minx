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

/* void task_save_publish_and_switch(uint64_t *save_rsp, task_t *prev,
 *                                   sched_cpu_t *c, uint64_t next_rsp,
 *                                   const page_table_t *next_tables,
 *                                   int32_t *prev_saved_in_user,
 *                                   int32_t *next_saved_in_user)
 *
 * Save the running task's context, publish it to a run queue, load another task's address
 * space, and switch to it, without ever returning to C.  Never returns.
 *
 * Why this cannot be task_switch(), and why the publication is inside it:
 *
 * The context of a task is its callee-saved registers *and its stack pointer*, and the
 * stack pointer is stored by the same act of pushing them.  So the save has to happen
 * before the publication: publishing first leaves a window in which the task is on
 * another processor's run queue while its saved_rsp still names an older stack frame.
 * That processor restores a stale RSP and the task resumes on a frame already in use,
 * which corrupts two live frames at once and hangs the machine in a way that looks like
 * a timer that stopped.
 *
 * Why it cannot come back to C either, which is the part that is easy to get wrong.
 * Saving and publishing as two calls does not work, because the six pushes sit *between*
 * the outer call's return address and the callback's.  With S the stack pointer of
 * schedule():
 *
 *     S-8    return address of `call task_save_publish_and_switch`
 *     S-16   rbp \
 *     S-24   rbx  |
 *     S-32   r12  |  the callee-saved set -- this is prev's saved context
 *     S-40   r13  |
 *     S-48   r14  |
 *     S-56   r15 /   <- saved_rsp points here
 *     S-64   return address of `call sched_publish_outgoing`
 *
 * Getting from the callback back to C means popping S-64 and then S-56, and popping S-56
 * destroys the context that was just saved -- so this function does not return to C at
 * all: after publishing, it switches.  That also settles where a preempted task resumes,
 * at S-8, which is *after* the publication, so the publication is never repeated.
 * Publishing from C after a save that returns would instead put the resume point *before*
 * the insertion, and every preemption would queue the task again.
 *
 * The address space is loaded *here*, between the publication and the switch, and not in
 * C before the call.  That ordering is forced by the same thing that forces everything
 * else here: a user task's saved_rsp is a user virtual address, and the pushes that save
 * its context have to happen while its own address space is still loaded.  Switching
 * first means pushing onto a user stack that the incoming task's tables do not map, which
 * is a page fault on the first push of every preemption of a ring-3 task.
 *
 * next_rsp and next_tables travel in %rcx and %r8 and are moved into %r12 and %r13 before
 * the first call, because the callbacks are C and may clobber the argument registers --
 * but %r12 and %r13 are callee-saved, so they survive.  Using them is free: prev's own
 * %r12 and %r13 are already on the stack by then.
 */
.global task_save_publish_and_switch
.type task_save_publish_and_switch, @function
task_save_publish_and_switch:
    /* Seven arguments, so the last one is on the stack and has to be read before anything
     * moves the stack pointer.  %r15 holds it: the outgoing task's own %r15 is pushed two
     * instructions below, and %r15 is callee-saved so both callbacks below keep it.
     *
     * Both flags are needed.  The save writes the outgoing task's -- recording whether it
     * was in ring 3 -- and the restore reads the incoming task's, which is what decides
     * between `ret` and `iretq`.  Passing only one of them compiles, links and boots, and
     * then reads the caller's return address as a pointer. */
    movq    8(%rsp), %r15       /* &next->saved_in_user */
    movq    %cs, %r11           /* caller-saved, and nothing else wants it yet */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)        /* final: from here on another processor may run it */
    movq    %rcx, %r12          /* prev's %r12 is saved above; the slot is now free */
    movq    %r8, %r13           /* likewise prev's %r13 */
    /* Was the outgoing task running in ring 3?  Then the CPU had already switched to a
     * kernel stack and pushed a full ring-3 return frame just above where we are now,
     * and that frame -- not a return address -- is what has to come back later. */
    movl    $0, (%r9)
    testq   $3, %r11
    jz      1f
    movl    $1, (%r9)
1:      /* rdi = save_rsp, rsi = prev, rdx = c: already the arguments the callback wants.
         * Still in prev's address space, which is where the outgoing task's stack lives. */
    call    sched_publish_outgoing
    movq    %r13, %rdi          /* next_tables */
    call    vmm_address_space_switch
    movq    %r12, %rsp          /* continue on the chosen task's stack */
    /* The flag pointer moves to %r11, which the pops below do not touch.  It cannot stay
     * in %r15: the first `popq %r15` is the *chosen* task's r15 -- zero for a task that
     * has never run -- and the test after the pops would then dereference zero.  That
     * faulted on the very first switch on the machine, which is at least a memorable
     * symptom for a mistake that is otherwise invisible. */
    movq    %r15, %r11
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    /* %rsp now points at the chosen task's resume point: a plain return address for a
     * task that was running in the kernel, and the CPU's own ring-3 frame for one that was
     * not.  `iretq` pops RIP, CS, RFLAGS, SS and RSP from exactly where `ret` would have
     * taken the first of them, so the two cases differ by one instruction. */
    cmpl    $0, (%r11)
    je      2f
    iretq
2:      ret
.size task_save_publish_and_switch, .-task_save_publish_and_switch

/* void task_restore_context(uint64_t new_rsp)
 *
 * The second half of a switch on its own: continue on the stack at new_rsp with that
 * task's callee-saved registers restored, returning into wherever it was.
 *
 * Does not return to its caller -- that is what a switch means.  There is deliberately
 * no `sti` here, unlike task_enter_first(): a task that has already run has live state
 * to return into, and that state decides whether interrupts should be on.  A task that
 * has never run has none, so it goes through task_enter_first() and the shim, where they
 * are turned on.
 *
 * Retained for a switch that cannot race, and *not* able to resume a ring-3 task: a task
 * interrupted in ring 3 has a CPU-pushed ring-3 frame above its callee-saved registers, and
 * `ret` pops that frame's RIP as a return address into ring 0.  task_save_publish_and_switch()
 * is the scheduler's switch and carries the flag that distinguishes the two; see
 * task_t.saved_in_user.  Kept here because a single-CPU switch with no possibility of a
 * user task involved is a real case, and removing it would remove the primitive that
 * documents why.
 */
.global task_restore_context
.type task_restore_context, @function
task_restore_context:
    movq    %rdi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
.size task_restore_context, .-task_restore_context

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

/* void task_enter_user(void)
 *
 * Reached by the `ret` at the end of a first switch onto a ring-3 task, so it runs on
 * the task's own stack with the ring-3 return frame already built underneath it.
 *
 * The kernel trampoline and the user frame are the same shape one level apart: six
 * callee-saved registers and a return address, then RFLAGS, CS, RIP, SS and RSP.  So
 * the job here is to leave the frame exactly where `iretq` expects it and get to the
 * trampoline, which is all of dropping the stack by the register frame.
 *
 * Interrupts are on before `iretq` rather than left to the frame.  RFLAGS in the frame
 * has IF set, and `iretq` does load it -- but a task whose very first instruction is
 * reached with IF still clear would run a long way before its first timer tick, and
 * the first thing it is likely to do is a syscall.
 */
.global task_enter_user
.type task_enter_user, @function
task_enter_user:
    sti
    /* Nothing to drop.  Restoring the saved context returned through its return-address
     * slot, so %rsp is now eight bytes past the top of that frame -- and the ring-3
     * return frame begins immediately there, with RIP as its first quadword.
     *
     * That is the arrangement `iretq` wants: it pops RIP, CS, RFLAGS, SS and RSP in that
     * order, each one quadword above the last, and enters ring 3 with %rsp pointing at
     * RIP.  An earlier version of this advanced %rsp by two quadwords to reach RIP in the
     * middle of the frame, which put RIP in the right slot and left CS holding the SS
     * selector, with SS and RSP read from above the frame -- a #GP out of `iretq` with no
     * ring-3 task anywhere.  task_create_user() writes the frame in the order the CPU
     * pops it for exactly this reason. */
    jmp    __enter_user
.size task_enter_user, .-task_enter_user

/* The kernel side of entering a ring-3 task.
 *
 * Reached by `jmp` from task_enter_user with %rsp already dropped past the register
 * frame, so %rsp points at the user return frame: RFLAGS, CS, RIP, SS, RSP.
 *
 * There is nothing to restore -- every general register was zero by construction, which
 * is the point: a task entering ring 3 starts with a register file the kernel chose,
 * not with whatever a kernel thread happened to be using.  So this is just the frame
 * handed back to the CPU.
 */
.global __enter_user
.type __enter_user, @function
__enter_user:
    iretq
.size __enter_user, .-__enter_user

.section .note.GNU-stack,"",@progbits
