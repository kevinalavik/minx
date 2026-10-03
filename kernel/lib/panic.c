#include "panic.h"

#include <stdarg.h>

#include "console.h"
#include "io.h"
#include "kprintf.h"
#include "serial.h"
#include "string.h"
#include "types.h"

/* QEMU's isa-debug-exit device: writing N makes QEMU exit with (N << 1) | 1.
 * 0 therefore means "clean shutdown" from the harness' point of view. */
#define QEMU_DEBUG_EXIT_PORT 0xf4

extern char __kernel_start[], __kernel_end[];

static bool panicking;

static bool in_kernel(uint64_t addr) {
    return addr >= (uint64_t)__kernel_start && addr < (uint64_t)__kernel_end;
}

void kboot_marker(char c) {
    outb(COM1, (uint8_t)c);
}

/* Register slots used by the dump below.  Index order matters. */
enum {
    REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP, REG_RSP,
    REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RIP, REG_CS,  REG_RFLAGS, REG_COUNT,
};

void dump_registers(void) {
    uint64_t regs[REG_COUNT];
    memzero(regs, sizeof(regs));

    /* Writing through one pointer keeps the register pressure legal: the
     * asm below clobbers every general purpose register it reads. */
    __asm__ volatile(
        "movq %%rax,  %[rax]\n\t"
        "movq %%rbx,  %[rbx]\n\t"
        "movq %%rcx,  %[rcx]\n\t"
        "movq %%rdx,  %[rdx]\n\t"
        "movq %%rsi,  %[rsi]\n\t"
        "movq %%rdi,  %[rdi]\n\t"
        "movq %%rbp,  %[rbp]\n\t"
        "movq %%rsp,  %[rsp]\n\t"
        "movq %%r8,   %[r8]\n\t"
        "movq %%r9,   %[r9]\n\t"
        "movq %%r10,  %[r10]\n\t"
        "movq %%r11,  %[r11]\n\t"
        "movq %%r12,  %[r12]\n\t"
        "movq %%r13,  %[r13]\n\t"
        "movq %%r14,  %[r14]\n\t"
        "movq %%r15,  %[r15]\n\t"
        "leaq 0f(%%rip), %%rax\n\t"
        "movq %%rax,  %[rip]\n\t"
        /* Segment and flag registers can only be read via a GPR, and RAX has
         * already been saved by this point so it doubles as the scratch. */
        "movq %%cs,   %%rax\n\t"
        "movq %%rax,  %[cs]\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax,  %[flags]\n\t"
        "0:\n\t"
        : [rax]"=m"(regs[REG_RAX]),   [rbx]"=m"(regs[REG_RBX]),
          [rcx]"=m"(regs[REG_RCX]),   [rdx]"=m"(regs[REG_RDX]),
          [rsi]"=m"(regs[REG_RSI]),   [rdi]"=m"(regs[REG_RDI]),
          [rbp]"=m"(regs[REG_RBP]),   [rsp]"=m"(regs[REG_RSP]),
          [r8]"=m"(regs[REG_R8]),     [r9]"=m"(regs[REG_R9]),
          [r10]"=m"(regs[REG_R10]),   [r11]"=m"(regs[REG_R11]),
          [r12]"=m"(regs[REG_R12]),   [r13]"=m"(regs[REG_R13]),
          [r14]"=m"(regs[REG_R14]),   [r15]"=m"(regs[REG_R15]),
          [rip]"=m"(regs[REG_RIP]),   [cs]"=m"(regs[REG_CS]),
          [flags]"=m"(regs[REG_RFLAGS])
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9",
          "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory");

    kprintf("  rip=%016lx rsp=%016lx rflags=%016lx cs=%04x\n",
            regs[REG_RIP], regs[REG_RSP], regs[REG_RFLAGS],
            (uint32_t)regs[REG_CS]);
    kprintf("  rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n",
            regs[REG_RAX], regs[REG_RBX], regs[REG_RCX], regs[REG_RDX]);
    kprintf("  rsi=%016lx rdi=%016lx rbp=%016lx\n",
            regs[REG_RSI], regs[REG_RDI], regs[REG_RBP]);
    kprintf("  r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n",
            regs[REG_R8], regs[REG_R9], regs[REG_R10], regs[REG_R11]);
    kprintf("  r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n",
            regs[REG_R12], regs[REG_R13], regs[REG_R14], regs[REG_R15]);
}

void dump_stack_trace(void) {
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));

    kprintf("  stack trace (kernel %016lx-%016lx):\n",
            (uint64_t)__kernel_start, (uint64_t)__kernel_end);

    int shown = 0;
    for (uint64_t i = 0; i < 64; i++) {
        uint64_t addr;
        memcpy(&addr, (const void *)(rsp + i * sizeof(uint64_t)),
               sizeof(addr));
        if (!in_kernel(addr))
            continue;
        kprintf("    [%02d] %016lx\n", shown, addr);
        shown++;
        if (shown >= 16)
            break;
    }
    if (shown == 0)
        kprintf("    (no kernel return addresses found)\n");
}

void panic(const char *fmt, ...) {
    cli();

    if (panicking)
        for (;;)
            __asm__ volatile("hlt");
    panicking = true;

    /* Raw output: the escape parser must not run while we are dying. */
    console_raw_write("\n=== KERNEL PANIC ===\n", 21);

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    console_raw_write("\n", 1);
    dump_registers();
    dump_stack_trace();
    console_raw_write("=== PANIC: halted ===\n", 23);

    /* Tell the harness this was a crash, then stop. */
    outb(QEMU_DEBUG_EXIT_PORT, 1);
    khang();
}

void khang(void) {
    cli();
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void kshutdown(int exit_code) {
    console_puts("\n[minx] shutting down\n");

    /* QEMU's isa-debug-exit device first: writing N makes it exit with
     * (N << 1) | 1, which is the only way the automated harness can tell a
     * clean shutdown from a triple fault (both exit 0 under -no-reboot).  On
     * real hardware nothing listens on port 0xf4 and the write is a no-op. */
    outb(QEMU_DEBUG_EXIT_PORT, (uint8_t)exit_code);

    /* Fall back to the ACPI power button for machines without the debug port. */
    outw(0x604, 0x2000);
    io_wait();
    outw(0xB004, 0x2000);
    io_wait();

    khang();
}