/* minx user program entry.
 *
 * RCX and R11 are undefined -- the kernel does not promise anything about them across a
 * process start -- and the `int $0x80` in every call below clobbers them, which is why
 * they are in each inline asm's clobber list.  The stack is aligned to 16 bytes here, as
 * the ABI requires at the first `call`.
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
    /* The loader builds the entry stack the SysV ABI describes: at the stack pointer,
     * `argc`; above it `argv[0]`, the NULL that ends argv, then envp, then the
     * auxiliary vector.  So the two arguments are two *different* words of it -- the one
     * at the stack pointer, and the eight bytes above that.
     *
     * Reading only the first and calling it argv is the mistake this comment replaces:
     * `minx_main(argc, argv)` then took a stack address as the argument count and a
     * zero as the argument vector, so the program reported an argc of 140737488350976
     * and `(none)` for argv[0] -- and passed, because the program handles argc == 0
     * gracefully.  A graceful fallback in the program hid a wrong stack layout in the
     * loader's caller. */
    movq    (%rsp), %rdi         /* argc */
    leaq    8(%rsp), %rsi        /* argv */
    xorl    %edx, %edx           /* auxv is AT_NULL, not a descriptor */
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
