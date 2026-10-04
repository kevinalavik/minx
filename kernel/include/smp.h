/* minx - symmetric multiprocessing.
 *
 * The bring-up half lives in kernel/arch/x86_64/smp.c; this is its interface.
 */
#ifndef MINX_SMP_H
#define MINX_SMP_H

#include <stdbool.h>
#include <stdint.h>

/* Stack for an application processor.  Every processor needs its own: two of them
 * sharing the boot processor's stack corrupt it the first time both take an
 * interrupt.
 *
 * 32 KiB, and not 16, because an application processor's stack is now a task's
 * stack: it runs whatever the scheduler hands it, including the kernel's own tests
 * with their buffers on it, and it carries an interrupt frame on top of all that.
 * Overflowing it does not fault cleanly -- the stack grows into the heap block
 * below it, and the first thing overwritten is somebody's return address, so the
 * symptom is an instruction fetch from the middle of the data segment. */
#define SMP_STACK_SIZE 32768

/* Start every processor the firmware described except the boot one, and leave
 * them in their idle loops.  Returns how many came up, or -1 if none did. */
int smp_start_all(void);

/* Bring one processor up with `entry` as its first instruction and `arg` in rsi.
 * `index` is the per-CPU slot to give it.  Returns 0 once it is running. */
int smp_start_cpu(uint32_t lapic_id, unsigned index, void (*entry)(void),
                  void *arg);

/* --------------------------------------------------------------- IPIs ----- */

/* Send `vector` to the processor with local APIC id `lapic_id`, or to every
 * processor including or excluding this one.
 *
 * Each returns once the local APIC reports the delivery complete.  That wait is
 * what makes it safe to send an IPI whose handler assumes the sender will stop and
 * not reuse the buffer it is about to write into: without it, the handler could
 * read the argument before the write that filled it had become visible.
 *
 * Interrupts are *not* disabled here.  The ICR write is already a locked
 * read-modify-write and the delivery-status wait is a plain read, so there is
 * nothing to lose by leaving interrupts on -- and a caller that has interrupts off
 * and sends an IPI to *itself* would hang, because the handler could not run until
 * the send returned.  A caller that needs the whole sequence atomic turns
 * interrupts off itself. */
int smp_send_ipi(uint8_t vector, uint32_t lapic_id);
int smp_ipi_self(uint8_t vector);
int smp_ipi_all_but_self(uint8_t vector);
int smp_ipi_all(uint8_t vector);

/* Send `vector` to the per-CPU slot `index`. */
int smp_ipi_cpu(unsigned index, uint8_t vector);

/* Run whatever `percpu_queue_work()` was given for slot `index`, by sending it a
 * reschedule IPI.  Running it inline if `index` is this processor, so it cannot
 * deadlock against a caller with interrupts off. */
int smp_kick_cpu(unsigned index);

/* The LAPIC ids of every online processor, so a caller that has to name each one
 * in turn does not have to reach into the per-CPU slots. */
unsigned smp_online_indices(unsigned *out, unsigned max);

/* Number of IPIs this processor has handled, for the self tests. */
uint64_t smp_ipis_received(void);

/* Install a handler for every IPI vector in vectors.h.  Done once, before any
 * processor is started, so no vector can arrive with nothing to run it. */
void smp_ipi_handlers_install(void);

#endif /* MINX_SMP_H */
