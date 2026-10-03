/* minx - interrupt/trap stubs.
 *
 * One stub per vector.  Each pushes the state described by regs_t and jumps to
 * the common dispatcher.  Vectors that the CPU delivers an error code for push
 * a dummy zero first so that the frame layout stays uniform.
 */

.section .text

.macro ISR_NOERR vec
    .global isr_stub_\vec
isr_stub_\vec:
    pushq $0
    pushq $\vec
    jmp isr_common
.endm

.macro ISR_ERR vec
    .global isr_stub_\vec
isr_stub_\vec:
    pushq $\vec
    jmp isr_common
.endm

/* CPU exceptions, 0..31.  8, 10-14 and 17 deliver an error code. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* Hardware IRQs remapped to 32..47 by the PIC.  These carry no error code. */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* Common tail: the stub has already pushed the vector (and error code).  Fill
 * in the remaining registers in exactly the order regs_t declares them, align
 * the stack for the System V ABI, call the dispatcher, and if it returns,
 * restore everything and iretq. */
.global isr_common
.type isr_common, @function
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Point RDI at the frame and switch to a clean compiler frame.  RBP is
     * already in the frame, so build a synthetic one for unwinding. */
    movq %rsp, %rdi
    movq %rsp, %rbp

    /* The System V ABI wants a 16-byte aligned stack at the call instruction,
     * i.e. RSP % 16 == 8 once inside the callee.  */
    andq $-16, %rsp
    pushq $0                      /* alignment padding */
    call isr_dispatch
    addq $16, %rsp

    movq %rbp, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp                /* drop the vector and error code */
    iretq
.size isr_common, . - isr_common