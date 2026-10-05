/* minx user program entry.
 *
 * The ABI hands a program argc in RDI and argv at [RSP], and nothing else.  RCX and R11
 * are undefined -- the kernel does not promise anything about them across a process
 * start -- so this clears them before any C code can assume anything, and puts the
 * stack pointer into a register so the frame below is 16-byte aligned as the ABI
 * requires at the first `call`.
 *
 * `xor %ebp, %ebp` is not decoration: it is the frame-pointer chain terminator, so a
 * debugger or a crash report walking it stops here instead of walking into whatever the
 * kernel happened to leave in RBP.
 */
.section .text
.globl _start
.type _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi          /* argv, which is also at [rsp] */
    xorl    %esi, %esi          /* no envp yet, and say so     */
    xorl    %edx, %edx          /* neither is AT_*             */
    andq    $-16, %rsp
    call    minx_main

    /* minx_main is not expected to return, but if it does its result becomes the
     * process's exit status rather than whatever happened to be left in a register.
     *
     * The exit is spelled out here rather than called through a C wrapper, and that is
     * not an accident: this is assembly, it cannot see a `static inline` in a header
     * that the compiler decided not to emit, and the symptom would be an undefined
     * reference at link time.  Doing it directly also puts the ABI in one place, which
     * is the point of having an entry point at all. */
    movl    %eax, %edi              /* SYS_EXIT's argument */
    movl    $4, %eax                /* SYS_EXIT */
    int     $0x80

    /* exit does not return either.  Looping here is a visible failure; falling off the
     * end would return into the kernel's trampoline with whatever was on the stack,
     * which is not. */
1:  jmp 1b
.size _start, .-_start

/* Statically linked, freestanding, and with no libgcc: the ELF loader does no
 * relocations and has no interpreter, so anything left undefined here would be a
 * relocation the loader would have to invent an answer for. */
.section .note.GNU-stack,"",@progbits
