#ifndef MINX_PERCPU_H
#define MINX_PERCPU_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

/* Per-processor state.
 *
 * The array is a static allocation rather than something handed out one CPU at a
 * time, because a processor's own area has to exist *before* it starts running:
 * the trampoline that brings it up reads and writes it before the kernel has had
 * any chance to allocate, and %gs has to point at something valid the instant
 * the first instruction executes.
 *
 * Each slot's first member is `self`, a pointer to its own `percpu_t`.  That is
 * what makes the accessor a single instruction: GS points at the slot, and
 * %gs:0 is that pointer.  Reading the slot's address out of an index would need
 * the index in a register on every call, which is exactly what a per-CPU
 * accessor exists to avoid.
 */
typedef struct {
    uint32_t lapic_id;
    bool     online;
    bool     started;
    uint64_t interrupts;     /* interrupts this CPU has handled */
    uint64_t idle_ticks;
    uint64_t context_switches;
    uint32_t sched_runqueue;  /* run queue this CPU last stole from */

    uint64_t stack_top;       /* where this CPU's stack was last left */
    uint32_t gs_installed;    /* GS points here */
} percpu_t;

typedef struct {
    percpu_t *self;     /* must stay first: read as %gs:0 */
    percpu_t  state;
} percpu_slot_t;

/* Called once per processor as it comes online. */
int  percpu_register(uint32_t lapic_id);
percpu_t *percpu_self(void);
percpu_t *percpu_by_index(unsigned index);

/* Address a processor's own per-CPU slot has to be given as IA32_GS_BASE. */
uint64_t percpu_gs_base(unsigned index);

/* Point this processor's GS at its own slot.  Idempotent, so the boot processor
 * can call it too and end up with exactly what an application processor has. */
void percpu_gs_install_self(void);

/* The index GS currently resolves to, found by reading back %gs:0.  This is the
 * one accessor that cannot use GS to find its own answer. */
unsigned percpu_self_index(void);

/* The index the boot processor registered as 0. */
unsigned percpu_boot_index(void);

unsigned percpu_count(void);          /* number of known processors      */
unsigned percpu_online_count(void);   /* number that have been started   */
uint32_t percpu_lapic_id(unsigned index);
uint64_t percpu_interrupts(unsigned index);
void     percpu_count_interrupt(void);
void     percpu_set_online(unsigned index, bool online);

/* Arm this processor's own LAPIC timer.
 *
 * The local APIC timer is per-CPU state: an application processor starts with
 * none of the boot processor's programming.  The 8253's interrupt is routed to
 * the boot processor, so nothing times on a processor that has not armed its own
 * counter -- and a processor that never arms one cannot notice a stop-machine
 * request either, which is how phase 5's idle loop would hang forever. */
void percpu_arm_local_timer(void);

/* Bring an application processor up with `entry` as its first instruction.
 * Returns 0 once the processor is running, -1 if it never arrived. */
int  percpu_start_ap(uint32_t lapic_id, void (*entry)(void *), void *arg);

/* The boot processor's index, always 0. */
#define PERCPU_BOOTSTRAP 0

#endif /* MINX_PERCPU_H */
