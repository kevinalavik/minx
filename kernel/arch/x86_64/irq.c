#include "irq.h"

#include "apic.h"
#include "cpu.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
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

/* Whether the IOAPIC, rather than the 8259s, is delivering the device lines. */
static bool via_ioapic;

/* Set once the IOAPIC has been given every line and the 8259s have been masked
 * for good.  From that point the IOAPIC's redirection entries are the only
 * thing that can deliver a device interrupt. */
void irq_set_ioapic_delivery(bool enabled) {
    via_ioapic = enabled;
}

bool interrupts_via_ioapic(void) {
    return via_ioapic;
}

int irq_enable(unsigned irq) {
    if (irq >= IRQ_MAX)
        return -1;

    if (via_ioapic) {
        /* A line whose IOAPIC pin turned out to belong to another device has no
         * redirection entry, so unmasking it would be a lie.  Failing is the
         * point: the alternative is a driver that believes it owns an
         * interrupt line that can never arrive. */
        if (!ioapic_legacy_line_routable(irq))
            return -1;

        ioapic_unmask_line(0, irq);
    } else {
        uint16_t mask = pic_irq_mask();
        mask &= (uint16_t)~(1u << irq);
        pic_set_mask(mask);
    }

    irq_slots[irq].enabled = true;
    return 0;
}

int irq_disable(unsigned irq) {
    if (irq >= IRQ_MAX)
        return -1;

    if (via_ioapic) {
        ioapic_mask_line(0, irq);
    } else {
        uint16_t mask = pic_irq_mask();
        mask |= (uint16_t)(1u << irq);
        pic_set_mask(mask);
    }

    irq_slots[irq].enabled = false;
    return 0;
}

/* Read the mask bit back out of the IOAPIC's redirection entry, the same way
 * pic_irq_mask() reads the 8259's OCW1: a write the hardware ignored has to be
 * visible, or the rest of the kernel would believe a line is unmasked when it
 * is not. */
bool irq_line_masked(unsigned irq) {
    if (irq >= IRQ_MAX)
        return true;

    if (via_ioapic)
        return ioapic_line_masked(0, irq);

    return (pic_irq_mask() & (1u << irq)) != 0;
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
    percpu_count_interrupt();

    /* Acknowledge the source before running the handler: a handler that blocked
     * would otherwise leave the interrupt pending and stop further IRQs.
     *
     * When the IOAPIC owns the lines, acknowledging means an end-of-interrupt to
     * the local APIC, not an OCW2 to the 8259 -- and, for a level-triggered
     * line, an explicit clear of the IOAPIC's Remote IRR bit, which is what
     * releases a handler that reads its device to completion. */
    if (interrupts_via_ioapic()) {
        ioapic_eoi(irq);
        lapic_eoi();
    } else {
        pic_eoi(irq);
    }

    irq_handler_t handler = irq_slots[irq].handler;
    if (handler != NULL)
        handler(regs);
    else if (irq_slots[irq].count <= 3)
        kprintf("[warn] unclaimed IRQ %u (%s) fired\n", irq,
                irq_name(irq));
}