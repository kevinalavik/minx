/* minx - exception and interrupt dispatch.
 *
 * Every vector funnels through isr_dispatch().  Exceptions are fatal until a
 * handler is installed for them, and they always report where the CPU was and
 * what it was doing, on the graphical console and on COM1 alike.
 */
#include "cpu.h"

#include "apic.h"
#include "boot.h"
#include "console.h"
#include "io.h"
#include "irq.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "signal.h"
#include "string.h"
#include "task.h"
#include "vectors.h"
#include "vmm.h"

static const char *const exception_names[32] = {
    "divide error",             "debug",
    "non-maskable interrupt",   "breakpoint",
    "overflow",                 "bound range exceeded",
    "invalid opcode",           "device not available",
    "double fault",             "coprocessor segment overrun",
    "invalid TSS",              "segment not present",
    "stack-segment fault",      "general protection fault",
    "page fault",               "reserved",
    "x87 floating-point",       "alignment check",
    "machine check",            "SIMD floating-point",
    "virtualization",           "control protection",
    "?",                        "?",
    "?",                        "?",
    "?",                        "?",
};

static isr_handler_t handlers[256];

void isr_install_handler(uint8_t vector, isr_handler_t handler) {
    if (vector < 32 && handler == NULL)
        return;
    handlers[vector] = handler;
}

/* Page-fault detail: decode the error code the CPU pushed. */
static void report_page_fault(regs_t *regs) {
    uint64_t addr = read_cr2();
    uint64_t code = regs->error_code;

    kprintf("  CR2 (fault address) = %#018lx\n", addr);
    kprintf("  error code %#lx: %s%s%s%s\n", code,
            (code & 0x1) ? "protection violation" : "page not present",
            (code & 0x2) ? ", write" : ", read",
            (code & 0x4) ? ", user" : ", supervisor",
            (code & 0x8) ? ", reserved bit set" : "");

    if (addr >= 0x0000000000001000ULL && addr < 0x0000000000100000ULL)
        kprintf("  -> low memory (below 1 MiB); the NULL page guard was hit\n");


}

void isr_report_exception(regs_t *regs) {
    uint64_t vector = regs->vector;
    const char *name = vector < 32 ? exception_names[vector] : "interrupt";

    kprintf("  vector %lu (%s), error code %#lx\n", vector, name,
            regs->error_code);
    kprintf("  rip=%#018lx cs=%#06lx rflags=%#018lx\n", regs->rip, regs->cs,
            regs->rflags);
    kprintf("  rsp=%#018lx rbp=%#018lx\n", regs->rsp, regs->rbp);
    kprintf("  rax=%#018lx rbx=%#018lx rcx=%#018lx\n", regs->rax, regs->rbx,
            regs->rcx);
    kprintf("  rdx=%#018lx rsi=%#018lx rdi=%#018lx\n", regs->rdx, regs->rsi,
            regs->rdi);
    kprintf("  r8 =%#018lx r9 =%#018lx r10=%#018lx\n", regs->r8, regs->r9,
            regs->r10);
    kprintf("  r11=%#018lx r12=%#018lx r13=%#018lx\n", regs->r11, regs->r12,
            regs->r13);
    kprintf("  r14=%#018lx r15=%#018lx\n", regs->r14, regs->r15);

    /* What the ring-3 entry saw, if a user task has been entered at all.  This is the
     * diagnostic that found the SS/RSP swap: the frame was verified correct from the
     * kernel's own alias of it, and the only way to tell whether %rsp pointed *at* that
     * alias when the CPU got there was to record what the CPU was about to read. */
    {
        extern uint64_t task_iret_frame[8];

        if (task_iret_frame[1] != 0)
            kprintf("  iret saw: cs=%#lx rsp=%#lx | rip=%#lx cs=%#lx rflags=%#lx "
                    "ss=%#lx rsp=%#lx\n",
                    (unsigned long)task_iret_frame[0],
                    (unsigned long)task_iret_frame[1],
                    (unsigned long)task_iret_frame[2],
                    (unsigned long)task_iret_frame[3],
                    (unsigned long)task_iret_frame[4],
                    (unsigned long)task_iret_frame[5],
                    (unsigned long)task_iret_frame[6]);
    }

    if (vector == 14)
        report_page_fault(regs);

    /* The top of a ring-3 task's own stack, when the exception came from one.
     *
     * The register dump above says what the CPU held; it cannot say where the program was
     * *going*, and for the failure this was written for -- a `longjmp()` that restored a
     * stack pointer one word too low, so the epilogue's `ret` popped a return address that
     * was not one and the program jumped to address 0 -- the answer was sitting in the
     * second stack slot.  Every register was correct at every instruction; only the return
     * chain was wrong, and nothing above says so.
     *
     * Read through the direct map of whatever frame backs that address rather than by
     * touching the user address: the page may not even be present, which is often why the
     * exception happened, and the kernel does not fault on its own diagnostic.  Sixteen
     * words is four return addresses' worth, which is enough to name the function. */
    if ((regs->cs & 3) != 0) {
        task_t *t = sched_current();

        if (t != NULL) {
            uint64_t base = regs->rsp & ~(uint64_t)(page_size - 1);
            uint64_t phys = vmm_translate_in(t->tables, base);

            if (phys != 0) {
                const uint64_t *w = (const uint64_t *)phys_to_virt(phys)
                                  + (regs->rsp - base) / 8;

                kprintf("  user stack at %#lx:\n", (unsigned long)regs->rsp);

                for (int i = 0; i < 16; i++)
                    kprintf("    +%#04lx  %#018lx\n", (unsigned long)(i * 8), w[i]);
            } else {
                kprintf("  user stack at %#lx: not mapped\n", (unsigned long)regs->rsp);
            }
        }
    }

    /* Where are we in the kernel?  */
    if (regs->rip >= (uint64_t)__kernel_start && regs->rip < (uint64_t)__kernel_end)
        kprintf("  -> kernel address (image %016lx..%016lx)\n",
                (uint64_t)__kernel_start, (uint64_t)__kernel_end);
    else
        kprintf("  -> NOT in the kernel image (%016lx..%016lx)\n",
                (uint64_t)__kernel_start, (uint64_t)__kernel_end);
}

/* The real dispatcher.  Static, because there is exactly one way in and signal delivery
 * must happen on the way out of *every* one of them. */
static void isr_dispatch_inner(regs_t *regs) {
    uint64_t vector = regs->vector;

    /* Did this arrive from ring 3?  The scheduler needs to know, and it cannot find out
     * for itself: the CPU has already switched to CPL 0, so the frame it pushed is a
     * ring-3 frame while the CS in hand belongs to the kernel.  See percpu_t.from_user. */
    if ((regs->cs & 3) != 0) {
        percpu_self()->from_user = 1;
        percpu_self()->user_frame = regs;
    }


    /* The remapped 8259 IRQs land on vectors 32..47, and the IOAPIC redirects
     * the same lines to the same vectors once it owns them. */
    if (vector >= IRQ_BASE_VECTOR && vector < IRQ_BASE_VECTOR + IRQ_MAX) {
        irq_dispatch(regs, (unsigned)(vector - IRQ_BASE_VECTOR));
        return;
    }

    /* A spurious interrupt is not a real interrupt: nothing was in flight, so
     * there is nothing to acknowledge and nothing to run.
     *
     * It still has to be acknowledged, and this is the one place in the kernel
     * where that is easy to get wrong.  A spurious interrupt sets an in-service
     * bit like any other, and it is the *only* kind whose handler has to clear it
     * explicitly -- the interrupt-acknowledge sequence an interrupt gate issues
     * suppresses it, because there was nothing to acknowledge in the first place.
     * Leaving it set makes the local APIC refuse every further interrupt of the
     * same priority on this processor, for ever: an application processor stops
     * taking its timer, stops taking IPIs, and sits in its idle loop with `hlt`
     * returning immediately because the request register still has something in
     * it.  Nothing crashes and nothing reports it; the processor is simply busy
     * and deaf.  So the end-of-interrupt is written here even though nothing was
     * delivered. */
    if (vector == VECTOR_LAPIC_SPURIOUS) {
        percpu_count_interrupt();
        lapic_eoi();
        return;
    }

    /* A write fault in a user address on a page this process shares with its twin is not
     * an error: it is the copy-on-write machinery being asked to do its job.  Handle it
     * here, before anything reports anything, and return -- the frame's RIP still names the
     * faulting instruction, because #PF is fault-like, so the tail's `iretq` re-executes it
     * against a page that is now private.
     *
     * Before this, a forked child that wrote to anything -- its stack, its data -- took a
     * #PF on a page the parent had every right to be using, and the only honest report the
     * kernel could make was "page fault", which says nothing about why.
     *
     * *Supervisor* faults are answered here too, and not as a courtesy.  The syscall
     * door writes the return value into the program's own frame, on the program's own
     * stack, which `fork` has just made read-only; that is a ring-0 store to a read-only
     * user page, and it is answered the same way a program's store is.  It is only raised
     * at all because `CR4.WP` is set (`vmm_init()`), and with it clear the store went
     * straight through into the frame the child shares -- so the child returned from
     * `fork()` holding the child's pid, took the parent's branch, and the two copies of
     * the program wrote over each other for as long as they ran.  A fault in ring 0 on an
     * address that is in no copy-on-write list still falls through to the report below,
     * which is right: that is a real fault in the kernel. */
    if (vector == 14) {
        task_t *t = sched_current();

        if (t != NULL && t->cow != NULL &&
            vmm_cow_fault(t->tables, t->cow, read_cr2())) {
            return;
        }
    }

    isr_handler_t handler = vector < 256 ? handlers[vector] : NULL;
    if (handler != NULL) {
        handler(regs);

        /* The ring-3 marker is *not* cleared here, and that is the half of this that is
         * easy to get wrong.
         *
         * Only the interrupt paths used to clear it, and they did it because they are the
         * ones that reach the scheduler, which consumes the marker and resets it.  The
         * syscall door never reaches the scheduler -- it clears IF for its whole length,
         * so a tick cannot arrive inside it and there is nothing to preempt -- so it left
         * the marker set, which handed the *next* save a ring-3 frame that was not there.
         *
         * Signal delivery needs the marker after the handler has run, because delivering
         * means patching the frame the tail is about to return through.  So the clearing
         * moved to isr_dispatch() below, which runs after everything: it is the one place
         * that sees both the handler and the delivery. */
        return;
    }

    if (vector < 32) {
        /* An unhandled CPU exception is a kernel bug; report it thoroughly and
         * stop rather than limping on with a corrupt state. */
        console_raw_write("\n", 1);
        isr_report_exception(regs);
        panic("unhandled CPU exception: vector %lu", vector);
    }

    kprintf("[warn] unexpected interrupt vector %lu ignored\n", vector);
}

/* The one place every return to ring 3 goes through.
 *
 * Delivery cannot live in the middle of the dispatcher: there are five exits from it (an
 * IRQ, the spurious vector, the copy-on-write fixup, a handled vector, and the panic
 * path), and a signal that is only delivered on one of them is a signal that is delivered
 * most of the time, which is indistinguishable from one that works.
 *
 * So the dispatcher is split rather than duplicated: the existing body runs first, and
 * then this wrapper looks for a signal to deliver and patches the live frame.  A task that
 * is switched away from here is switched away with the frame already patched, because the
 * scheduler saves the frame -- and the frame is what the patch is in.
 *
 * `percpu_self()->from_user` is read and then cleared here, for the same reason the
 * scheduler consumes it: it is "the outermost interrupted context was ring 3", and it is
 * the only record of that which is still true by the time the tail runs. */
void isr_dispatch(regs_t *regs) {
    isr_dispatch_inner(regs);

    /* Read *after* the dispatcher, not before: it is the dispatcher that records that the
     * interrupt arrived from ring 3, and reading it earlier is reading the last
     * interrupt's answer to this one's question.  On a syscall path -- which is where
     * signals are delivered from -- that is always zero, and no signal would ever arrive.
     *
     * Cleared afterwards for the reason the handler path no longer clears it: this is the
     * last instruction before either the scheduler consumes the marker or the tail's
     * `iretq` returns to ring 3 without it. */
    if (percpu_self()->from_user != 0)
        signal_deliver_on_return_to_user(regs);

    percpu_self()->from_user = 0;
    percpu_self()->user_frame = NULL;
}