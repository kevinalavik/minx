#include "percpu.h"

#include "acpi.h"
#include "apic.h"
#include "boot.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "types.h"

/* The per-CPU area the GS base points at.  Until phase 4 gives each CPU its own
 * stack and its own copy, every processor reads this one instance, which is
 * correct precisely because only the boot CPU is running. */
static percpu_t percpu_area[MAX_CPU_COUNT];
static unsigned percpu_registered;

int percpu_register(uint32_t lapic_id) {
    if (percpu_registered >= MAX_CPU_COUNT)
        return -1;

    unsigned idx = percpu_registered++;
    percpu_area[idx].lapic_id = lapic_id;
    percpu_area[idx].online = false;
    percpu_area[idx].started = false;
    return (int)idx;
}

percpu_t *percpu_by_index(unsigned index) {
    return index < MAX_CPU_COUNT ? &percpu_area[index] : NULL;
}

percpu_t *percpu_self(void) {
    return &percpu_area[PERCPU_BOOTSTRAP];
}

unsigned percpu_count(void) {
    return percpu_registered;
}

unsigned percpu_online_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < percpu_registered; i++) {
        if (percpu_area[i].online)
            n++;
    }
    return n;
}

uint32_t percpu_lapic_id(unsigned index) {
    return index < percpu_registered ? percpu_area[index].lapic_id : 0;
}

uint64_t percpu_interrupts(unsigned index) {
    return index < percpu_registered ? percpu_area[index].interrupts : 0;
}

void percpu_count_interrupt(void) {
    percpu_self()->interrupts++;
}

int percpu_start_ap(uint32_t lapic_id, void (*entry)(void *), void *arg) {
    (void)lapic_id;
    (void)entry;
    (void)arg;

    /* Starting an application processor needs the LAPIC to be reachable, and the
     * bootloader's page tables deliberately do not map MMIO, so there is nothing
     * to send the startup IPI through yet.  Phase 3 installs page tables that map
     * the APIC window and this becomes a real INIT-SIPI-SIPI sequence. */
    kprintf("[warn] percpu: cannot start LAPIC %u yet; the APIC is not "
            "mapped by the bootloader's page tables\n", lapic_id);
    return -1;
}