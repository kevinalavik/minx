/* minx - the entry point a libc-linked program starts at.
 *
 * `user/common/start.S` is the entry point for a program with *no* libc, and it calls
 * `minx_main` and then makes the exit syscall itself.  This is the same logic for a program
 * that has one, and the two exist side by side deliberately:
 *
 *   - a ring-3 test program is written against the raw door, so a libc bug cannot be what
 *     the test is measuring;
 *   - a normal program is written against libc, so a libc bug is a bug like any other.
 *
 * Keeping the freestanding path working is not nostalgia.  It is the only way to check
 * something about the kernel -- the syscall door, the loader, the signal trampoline -- with
 * nothing in between to be wrong.
 *
 * `__libc_start_main()` takes the stack pointer rather than the three arguments separately,
 * for the same reason `user/common/start.S` reads `%rsp`: the stack the loader built *is*
 * the arguments, and reading it in one place is what stops the two entry stubs from
 * disagreeing about its shape.
 *
 * The exit syscall number is a literal here, as it is in `start.S`, and for the same reason:
 * this is assembly, it cannot see a C header's macro, and the generated `minx_syscall.inc`
 * is there so that the literal is *generated*.  Nothing about it is written by hand.
 */
#include "minx_syscall.inc"

.section .text
.globl _start
.type _start, @function
_start:
    xorl    %ebp, %ebp          /* the frame-pointer chain terminator, so a crash report
                                 * walking it stops here rather than walking into whatever
                                 * the kernel left in RBP */
    movq    %rsp, %rdi          /* the stack the loader built, which *is* the arguments */
    andq    $-16, %rsp          /* 16-byte aligned at the call, as the ABI requires */
    call    __libc_start_main

    /* `__libc_start_main()` ends in `exit()` and does not return.  Reaching here would mean
     * that changed, and the right thing to do about it is to exit the way the kernel was
     * asked to: with the value in EAX, which is what `main()` returned. */
    movl    %eax, %edi
    movl    $SYS_EXIT, %eax
    int     $0x80

    /* Exit does not return either.  Looping is a visible failure; falling off the end would
     * return into the kernel's trampoline with whatever was on the stack, which is not. */
1:  jmp 1b
.size _start, .-_start

/* Statically linked, freestanding, and with no libgcc: the ELF loader does no relocations
 * and has no interpreter, so anything left undefined here would be a relocation the loader
 * would have to invent an answer for. */
.section .note.GNU-stack,"",@progbits