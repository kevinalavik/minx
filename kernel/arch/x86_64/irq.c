#include "irq.h"

#include "apic.h"
#include "cpu.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "types.h"

typedef struct {
    irq_handler_t handler;
    const char   *name;
    uint64_t      count;
    bool          enabled;
} irq_slot_t;

static irq_slot_t irq_slots[IRQ_MAX];

/* The 8259s are cascaded with the slave on master line 2. */
#define PIC_SLAVE_ON_MASTER 2

int irq_register(unsigned irq, irq_handler_t handler, const char *name) {
    if (irq >= IRQ_MAX)
        return -1;

    irq_slots[irq].handler = handler;
    irq_slots[irq].name = name != NULL ? name : "unnamed";
    irq_slots[irq].count = 0;
    return 0;
}

int irq_unregister(unsigned irq) {
    if (irq >= IRQ_MAX)
        return -1;
    irq_slots[irq].handler = NULL;
    irq_slots[irq].name = NULL;
    return 0;
}

irq_handler_t irq_handler(unsigned irq) {
    return irq < IRQ_MAX ? irq_slots[irq].handler : NULL;
}

const char *irq_name(unsigned irq) {
    if (irq >= IRQ_MAX)
        return "?";
    return irq_slots[irq].name != NULL ? irq_slots[irq].name : "(unclaimed)";
}

uint64_t irq_count(unsigned irq) {
    return irq < IRQ_MAX ? irq_slots[irq].count : 0;
}

bool irq_enabled(unsigned irq) {
    if (irq >= IRQ_MAX)
        return false;
    return irq_slots[irq].enabled;
}

int irq_enable(unsigned irq) {
    if (irq >= IRQ_MAX)
        return -1;

    uint16_t mask = pic_irq_mask();
    mask &= (uint16_t)~(1u << irq);
    pic_set_mask(mask);
    irq_slots[irq].enabled = true;
    return 0;
}

int irq_disable(unsigned irq) {
    if (irq >= IRQ_MAX)
        return -1;

    uint16_t mask = pic_irq_mask();
    mask |= (uint16_t)(1u << irq);
    pic_set_mask(mask);
    irq_slots[irq].enabled = false;
    return 0;
}

void pic_eoi(unsigned irq) {
    /* The slave hangs off the master, so its interrupt has to be acknowledged
     * first or the master will not finish the sequence. */
    if (irq >= 8)
        outb(0xa0, 0x20);
    outb(0x20, 0x20);
}

void irq_dispatch(struct regs *regs, unsigned irq) {
    if (irq >= IRQ_MAX)
        return;

    irq_slots[irq].count++;

    /* Acknowledge before running the handler: a handler that blocks would
     * otherwise leave the PIC asserting the line and stop further IRQs. */
    pic_eoi(irq);

    irq_handler_t handler = irq_slots[irq].handler;
    if (handler != NULL)
        handler(regs);
    else if (irq_slots[irq].count <= 3)
        kprintf("[warn] unclaimed IRQ %u (%s) fired\n", irq,
                irq_name(irq));
}