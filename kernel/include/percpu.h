/* minx - per-processor state.
 *
 * Phase 2 only has the boot processor; phase 4 fills in the rest and switches
 * the accessors over to a GS-based lookup.  Keeping the array and the accessors
 * from the start means the SMP work has one place to change.
 */
#ifndef MINX_PERCPU_H
#define MINX_PERCPU_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

typedef struct {
    uint32_t lapic_id;
    bool     online;
    bool     started;
    uint64_t interrupts;     /* interrupts this CPU has handled */
    uint64_t idle_ticks;
    uint64_t context_switches;
    uint32_t sched_runqueue;  /* run queue this CPU last stole from */
} percpu_t;

/* Called once per processor as it comes online. */
int  percpu_register(uint32_t lapic_id);
percpu_t *percpu_self(void);
percpu_t *percpu_by_index(unsigned index);

unsigned percpu_count(void);          /* number of known processors      */
unsigned percpu_online_count(void);   /* number that have been started   */
uint32_t percpu_lapic_id(unsigned index);
uint64_t percpu_interrupts(unsigned index);
void     percpu_count_interrupt(void);

/* Bring an application processor up.  Phase 4 does the real work; until then it
 * reports that the processor is not yet available, so callers that ask for more
 * CPUs than the kernel supports get a clear answer rather than a hang. */
int  percpu_start_ap(uint32_t lapic_id, void (*entry)(void *), void *arg);

/* The boot processor's index, always 0. */
#define PERCPU_BOOTSTRAP 0

#endif /* MINX_PERCPU_H */