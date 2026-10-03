/* minx - interrupt routing.
 *
 * Vectors 32..47 are the legacy 8259 IRQs after the PICs have been remapped;
 * phase 4 moves them onto the IOAPIC, but until then this is the only path an
 * interrupt can take, and the PS/2 keyboard depends on it.
 */
#ifndef MINX_IRQ_H
#define MINX_IRQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define IRQ_BASE_VECTOR 32
#define IRQ_MAX         16
#define IRQ_NONE        0xffffffffu

typedef void (*irq_handler_t)(struct regs *regs);

/* Install a handler for IRQ `irq` (0..15), which lands on vector
 * IRQ_BASE_VECTOR + irq. */
int  irq_register(unsigned irq, irq_handler_t handler, const char *name);
int  irq_unregister(unsigned irq);
irq_handler_t irq_handler(unsigned irq);
const char *irq_name(unsigned irq);

bool irq_enabled(unsigned irq);
int  irq_enable(unsigned irq);
int  irq_disable(unsigned irq);

/* Counters, so the self test can prove an interrupt really fired. */
uint64_t irq_count(unsigned irq);

/* Called by the common interrupt stub; acknowledges the source and dispatches. */
void irq_dispatch(struct regs *regs, unsigned irq);

/* Master/slave cascade line, exposed because the keyboard driver sometimes has
 * to tell whether an IRQ2 was a spurious slave interrupt. */
void pic_eoi(unsigned irq);

#endif /* MINX_IRQ_H */