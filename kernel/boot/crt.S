/* minx kernel entry point
 *
 * The bootloader drops us here in 64-bit long mode with interrupts disabled,
 * paging already enabled, and a low identity map that we are about to throw
 * away.  Everything below must be freestanding: no libc, no red zone, no SSE.
 */

.section .text.boot, "ax"
.code64
.global _start
.type _start, @function

#ifdef MINX_BOOT_TRACE
/* Early boot markers written straight to COM1, before any of the C runtime
 * exists.  They make it obvious how far the kernel got even when a later stage
 * faults before it can say anything useful.  Enable with MINX_BOOT_TRACE=1. */
.macro BOOT_MARKER ch
    movw $0x3f8, %dx
    movb $\ch, %al
    outb %al, %dx
.endm
#else
.macro BOOT_MARKER ch
.endm
#endif

_start:
    BOOT_MARKER '0'

    cli
    cld

    /* Park the bootloader handover arguments in callee-saved registers so the
     * .bss clear below can reuse RDI/RCX freely. */
    movq %rdi, %r12            /* limine_mp_info * (or the response block) */
    movq %rsi, %r13            /* block of response structs */
    movq %rdx, %r14            /* block of request id structs  */

    BOOT_MARKER 'a'

    /* Zero the .bss: RDI = start, RCX = length, AL = 0. */
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb

    BOOT_MARKER 'b'

    /* The stack the bootloader left us on lives in the higher-half direct map,
     * which the memory manager will hand out later, so kmain() switches to a
     * heap-allocated kernel stack as soon as the allocator is up. */

    xorq %rbp, %rbp

    BOOT_MARKER 'c'

    movq %r12, %rdi
    movq %r13, %rsi
    movq %r14, %rdx

    BOOT_MARKER 'd'

    call kmain

    /* kmain() must never return; if it does, halt loudly. */
    call khang

.Lhang:
    cli
    hlt
    jmp .Lhang
.size _start, . - _start

/* void kstack_enter(void *top, void (*entry)(void)) -- noreturn
 *
 * Switch to a freshly allocated kernel stack and jump to *entry.  kmain() never
 * returns: its frame is abandoned together with the old bootloader stack.
 *
 *   RDI = new stack top (16-byte aligned)
 *   RSI = entry point
 */
.global kstack_enter
.type kstack_enter, @function
kstack_enter:
    andq $-16, %rdi
    subq $8, %rdi               /* room for the synthetic return address */
    movq %rdi, %rsp
    pushq %rsi                  /* the ret below lands on *entry* */
    xorq %rbp, %rbp
    ret
.size kstack_enter, . - kstack_enter