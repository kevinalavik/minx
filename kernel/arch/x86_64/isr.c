/* minx - exception and interrupt dispatch.
 *
 * Every vector funnels through isr_dispatch().  Exceptions are fatal until a
 * handler is installed for them, and they always report where the CPU was and
 * what it was doing, on the graphical console and on COM1 alike.
 */
#include "cpu.h"

#include "boot.h"
#include "console.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

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

    if (vector == 14)
        report_page_fault(regs);

    /* Where are we in the kernel?  */
    if (regs->rip >= (uint64_t)__kernel_start && regs->rip < (uint64_t)__kernel_end)
        kprintf("  -> kernel address (image %016lx..%016lx)\n",
                (uint64_t)__kernel_start, (uint64_t)__kernel_end);
    else
        kprintf("  -> NOT in the kernel image (%016lx..%016lx)\n",
                (uint64_t)__kernel_start, (uint64_t)__kernel_end);
}

void isr_dispatch(regs_t *regs) {
    uint64_t vector = regs->vector;

    isr_handler_t handler = vector < 256 ? handlers[vector] : NULL;
    if (handler != NULL) {
        handler(regs);
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