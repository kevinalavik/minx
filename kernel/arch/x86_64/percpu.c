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

/* Point GS at slot `index`.
 *
 * This takes the index rather than finding it, because the other way round is not
 * possible at the point an application processor needs it: it has just reloaded
 * its segment registers, and loading a segment selector in 64-bit mode zeroes
 * that segment's hidden base -- including GS's -- so %gs:0 no longer resolves to
 * the slot the accessor is supposed to read.  A caller that knows its index
 * installs it without consulting a register that has just been invalidated. */
void percpu_gs_install_index(unsigned index) {
    percpu_slot_t *slot = &slots[index];
    wrmsr(MSR_GS_BASE, (uint64_t)slot);
    slot->state.gs_installed = 1;
}

void percpu_gs_install_self(void) {
    percpu_gs_install_index(percpu_self_index());
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

uint64_t percpu_idle_ticks(unsigned index) {
    return (index < registered) ? slots[index].state.idle_ticks : 0;
}

bool percpu_is_online(unsigned index) {
    return index < registered && slots[index].state.online;
}

bool percpu_is_started(unsigned index) {
    return index < registered && slots[index].state.started;
}

unsigned percpu_index_for_lapic_id(uint32_t lapic_id) {
    for (unsigned i = 0; i < registered; i++)
        if (slots[i].state.lapic_id == lapic_id)
            return i;
    return MAX_CPU_COUNT;
}

int percpu_queue_work(unsigned index, void (*fn)(void *), void *arg) {
    if (index >= registered)
        return -1;

    percpu_slot_t *slot = &slots[index];

    /* Claim the slot before filling it.  A second sender that finds it claimed
     * gets -1 instead of overwriting work that has not run yet, which would be the
     * quiet kind of wrong. */
    uint32_t zero = 0;
    if (!__atomic_compare_exchange_n(&slot->state.pending, &zero, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -1;

    slot->state.pending_fn = fn;
    slot->state.pending_arg = arg;
    /* The store to `pending` above releases the two above it, so a processor that
     * sees the flag set also sees the function and its argument. */
    return 0;
}

void percpu_run_pending(void) {
    percpu_t *self = percpu_self();

    if (self == NULL)
        return;

    for (;;) {
        if (__atomic_load_n(&self->pending, __ATOMIC_ACQUIRE) == 0)
            return;

        void (*fn)(void *) = self->pending_fn;
        void *arg = self->pending_arg;

        /* Clear before running, so work queued *by* this work finds a free slot
         * rather than being refused because of the work already running. */
        __atomic_store_n(&self->pending, 0, __ATOMIC_RELEASE);

        if (fn != NULL)
            fn(arg);
    }
}

bool percpu_work_pending(unsigned index) {
    if (index >= registered)
        return false;
    return __atomic_load_n(&slots[index].state.pending, __ATOMIC_ACQUIRE) != 0;
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

void percpu_count_ipi(void) {
    percpu_self()->ipis++;
}

uint64_t percpu_ipis(unsigned index) {
    return (index < registered) ? slots[index].state.ipis : 0;
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
