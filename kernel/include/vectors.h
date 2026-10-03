/* minx - the interrupt vector map.
 *
 * One table, used by the IDT, the IOAPIC redirection entries, the LAPIC and the
 * IPI helpers, so a vector can never be handed out twice by accident.  The same
 * layout is documented for userspace in docs/SYSCALLS.md.
 *
 *   0x00-0x1f   CPU exceptions
 *   0x20-0x2f   device interrupt lines 0-15
 *                 - delivered by the 8259s at vectors 0x20-0x2f when there is
 *                   no IOAPIC,
 *                 - delivered by IOAPIC GSI 0-15 at the same vectors once the
 *                   IOAPIC is up.  The 8259s stay fully masked from then on, so
 *                   a line has exactly one owner at a time.
 *   0x30-0x3f   kernel IPIs and local APIC sources
 *   0xf0-0xff   architectural delivery modes: 0xfd INIT, 0xfe startup,
 *               0xff shutdown (handled by the CPU, never in the IDT)
 */
#ifndef MINX_VECTORS_H
#define MINX_VECTORS_H

/* First vector for the sixteen device lines.  Matches IRQ_BASE_VECTOR. */
#define VECTOR_IRQ0        0x20
#define VECTOR_IRQ_MAX      0x10

/* Local APIC sources and inter-processor interrupts. */
#define VECTOR_LAPIC_TIMER  0x30   /* the preemption tick                  */
#define VECTOR_LAPIC_ERROR  0x31
#define VECTOR_LAPIC_SPURIOUS 0x32
#define VECTOR_IPI_TLB      0x33   /* shoot down another CPU's TLB         */
#define VECTOR_IPI_RESCHED  0x34   /* "you have work / preemption pending" */
#define VECTOR_IPI_HALT     0x35   /* panic: stop every other CPU          */
#define VECTOR_IPI_WAKE     0x36   /* leave the idle loop                  */
#define VECTOR_IPI_NMI      0x37   /* kernel NMI handler                  */

#define VECTOR_IPI_FIRST    VECTOR_IPI_TLB
#define VECTOR_IPI_LAST     VECTOR_IPI_WAKE

/* Architectural vectors delivered by the local APIC itself. */
#define VECTOR_LAPIC_INIT     0xfd
#define VECTOR_LAPIC_STARTUP  0xfe
#define VECTOR_LAPIC_SHUTDOWN 0xff

#endif /* MINX_VECTORS_H */