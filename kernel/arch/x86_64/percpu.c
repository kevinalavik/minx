#include "percpu.h"

#include "acpi.h"
#include "apic.h"
#include "boot.h"
#include "cpu.h"
#include "io.h"
#include "timer.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "types.h"

/* A static array, not something handed out one processor at a time: a
 * processor's slot has to exist before it runs, because the trampoline that
 * brings it up reads and writes it before any allocation could have happened.
 * Sixty-four entries covers a long enough to make a machine that reports more
 * say so rather than being believed. */
static percpu_slot_t slots[MAX_CPU_COUNT];
static unsigned registered;
static unsigned boot_index = PERCPU_BOOTSTRAP;

/* --- GS ---------------------------------------------------------------- */

/* The current processor's own slot, read straight out of GS.
 *
 * %gs:0 is the slot's `self` member, which points at the same object.  Two
 * loads, no index, no table walk: the point of a per-CPU accessor is that the
 * compiler can inline it into whatever is counting interrupts, and an accessor
 * that needs a register the caller is already using does not get inlined. */
percpu_t *percpu_self(void) {
    percpu_t *self;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(self) :: "memory");
    return self;
}

uint64_t percpu_gs_base(unsigned index) {
    return index < MAX_CPU_COUNT ? (uint64_t)&slots[index] : 0;
}

void percpu_gs_install_self(void) {
    percpu_slot_t *slot = &slots[percpu_self_index()];
    wrmsr(MSR_GS_BASE, (uint64_t)slot);
    slot->state.gs_installed = 1;
}

unsigned percpu_self_index(void) {
    /* The one accessor that cannot use GS to find its own answer: it is what
     * reads GS.  What GS:0 holds is the slot's *state* pointer, so the answer is
     * that pointer compared against each slot's state -- comparing it against the
     * slot addresses instead never matches anything, which turns a correct GS
     * into a report that GS is broken. */
    percpu_t *state;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(state) :: "memory");

    for (unsigned i = 0; i < MAX_CPU_COUNT; i++)
        if (&slots[i].state == state)
            return i;

    /* GS is not pointing at a slot this kernel knows: either it was never set,
     * or something clobbered it.  Falling back to the boot processor is the
     * safe answer -- it is a real slot with a real stack behind it -- and the
     * boot log says so. */
    kprintf("[warn] percpu: %%gs does not point at a known slot; assuming "
            "processor %u\n", PERCPU_BOOTSTRAP);
    return PERCPU_BOOTSTRAP;
}

/* --- registration ------------------------------------------------------ */

int percpu_register(uint32_t lapic_id) {
    if (registered >= MAX_CPU_COUNT)
        return -1;

    unsigned idx = registered++;
    memset(&slots[idx], 0, sizeof(slots[idx]));
    slots[idx].self = &slots[idx].state;
    slots[idx].state.lapic_id = lapic_id;
    slots[idx].state.online = false;
    slots[idx].state.started = false;

    /* The boot processor has to have GS pointing at its slot before anything can
     * call percpu_self(), which is the first interrupt handler.  Registering is
     * the first thing that happens to it, so installing here closes the window
     * rather than leaving a caller to remember. */
    if (idx == boot_index)
        wrmsr(MSR_GS_BASE, (uint64_t)&slots[idx]);

    return (int)idx;
}

percpu_t *percpu_by_index(unsigned index) {
    return index < MAX_CPU_COUNT ? &slots[index].state : NULL;
}

unsigned percpu_boot_index(void) {
    return boot_index;
}

unsigned percpu_count(void) {
    return registered;
}

unsigned percpu_online_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < registered; i++) {
        if (slots[i].state.online)
            n++;
    }
    return n;
}

uint32_t percpu_lapic_id(unsigned index) {
    return index < registered ? slots[index].state.lapic_id : 0;
}

uint64_t percpu_interrupts(unsigned index) {
    return index < registered ? slots[index].state.interrupts : 0;
}

void percpu_set_online(unsigned index, bool online) {
    if (index < registered)
        slots[index].state.online = online;
}

void percpu_count_interrupt(void) {
    percpu_self()->interrupts++;
}

void percpu_arm_local_timer(void) {
    /* Arming another processor's timer is not possible through the boot
     * processor's window, so this has to be the processor's own code running on
     * it.  The 8253's line is routed to the boot processor's local APIC, so
     * nothing else would ever tick here. */
    lapic_timer_arm_local();
}

int percpu_start_ap(uint32_t lapic_id, void (*entry)(void *), void *arg) {
    (void)lapic_id;
    (void)entry;
    (void)arg;

    kprintf("[warn] percpu: percpu_start_ap() is a placeholder; "
            "smp_start_all() in kernel/arch/x86_64/smp.c does the work\n");
    return -1;
}
