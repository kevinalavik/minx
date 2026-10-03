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
 * interrupt. */
#define SMP_STACK_SIZE 16384

/* Start every processor the firmware described except the boot one, and leave
 * them in their idle loops.  Returns how many came up, or -1 if none did. */
int smp_start_all(void);

/* Bring one processor up with `entry` as its first instruction and `arg` in rsi.
 * `index` is the per-CPU slot to give it.  Returns 0 once it is running. */
int smp_start_cpu(uint32_t lapic_id, unsigned index, void (*entry)(void),
                  void *arg);

#endif /* MINX_SMP_H */
