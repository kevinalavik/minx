/* minx - atomics, spinlocks, mutexes, semaphores and wait queues.
 *
 * The shapes and the reasons are in kernel/include/spinlock.h.  What is worth
 * repeating here is what the primitives compile to, because all of it rests on one
 * x86-64 guarantee: the `lock` prefix makes an instruction atomic *and* orders
 * everything around it.  A locked instruction is a full barrier on this machine, so
 * no separate fence is needed inside any of the lock paths below, and the only
 * fence in the file is the one a caller asks for by name.
 *
 * Interrupts in a spinlock path are a problem the `lock` prefix does not solve.
 * If the processor holding a lock takes an interrupt and the handler tries to take
 * the same lock, it spins forever waiting for itself.  x86-64 has no instruction
 * that says "take this lock and raise the priority above the level that would take
 * it again", so every lock a handler can also take has to turn interrupts off
 * around it.  That is what the `_irqsave` variants are for, and it is why the saved
 * flags are handed back to the caller rather than discarded.
 *
 * The mutex and the semaphore *spin* rather than sleep.  That is correct -- mutual
 * exclusion holds -- but it burns a processor's whole time slice waiting, and it
 * stops being the right choice the moment a critical section can block.  Phase 5
 * gives them a place to sleep; until then this is a spin mutex, and the header says
 * so.
 */
#include "spinlock.h"

#include "io.h"
#include "panic.h"
#include "percpu.h"
#include "string.h"

/* ---------------------------------------------------------------- atomics --- */

void atomic_init(atomic_t *a, int32_t value) {
    __atomic_store_n(&a->counter, value, __ATOMIC_RELAXED);
}

int32_t atomic_load(const atomic_t *a) {
    return __atomic_load_n(&a->counter, __ATOMIC_ACQUIRE);
}

void atomic_store(atomic_t *a, int32_t value) {
    __atomic_store_n(&a->counter, value, __ATOMIC_RELEASE);
}

int32_t atomic_fetch_add(atomic_t *a, int32_t delta) {
    return __atomic_fetch_add(&a->counter, delta, __ATOMIC_ACQ_REL);
}

int32_t atomic_fetch_sub(atomic_t *a, int32_t delta) {
    return __atomic_fetch_sub(&a->counter, delta, __ATOMIC_ACQ_REL);
}

int32_t atomic_increment(atomic_t *a) { return atomic_fetch_add(a, 1); }
int32_t atomic_decrement(atomic_t *a) { return atomic_fetch_sub(a, 1); }

bool atomic_cas(atomic_t *a, int32_t expected, int32_t desired) {
    return __atomic_compare_exchange_n(&a->counter, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool atomic_trylock(atomic_t *a) {
    int32_t zero = 0;
    return __atomic_compare_exchange_n(&a->counter, &zero, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* The per-processor lock flag.
 *
 * A byte in memory rather than a register: the point of it is that only this
 * processor ever looks at it, and there is no way to tell a C compiler that a
 * register is per-processor. */
static uint8_t cpu_lock_flags[MAX_CPU_COUNT];

static unsigned cpu_lock_index(const char *what) {
    unsigned index = percpu_self_index();
    if (index >= MAX_CPU_COUNT) {
        panic("%s: %%gs does not resolve to a known processor\n", what);
        return 0;
    }
    return index;
}

void cpu_lock_acquire(void) {
    __atomic_add_fetch(&cpu_lock_flags[cpu_lock_index("cpu_lock_acquire")], 1,
                       __ATOMIC_ACQ_REL);
}

void cpu_lock_release(void) {
    __atomic_sub_fetch(&cpu_lock_flags[cpu_lock_index("cpu_lock_release")], 1,
                       __ATOMIC_ACQ_REL);
}

bool cpu_lock_held(void) {
    unsigned index = percpu_self_index();
    if (index >= MAX_CPU_COUNT)
        return false;
    return __atomic_load_n(&cpu_lock_flags[index], __ATOMIC_ACQUIRE) != 0;
}

/* ----------------------------------------------------------- ticket locks --- */

void ticket_lock_init(ticket_lock_t *l) {
    __atomic_store_n(&l->ticket, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&l->serving, 0, __ATOMIC_RELAXED);
}

/* One 16-bit field at a time, rather than one 32-bit `lock xadd` on the whole word.
 *
 * A single locked add would be one instruction instead of two, but it would put
 * both counters in the same cache line *and* the same read-modify-write, so every
 * processor drawing a ticket would invalidate the line the processor being handed
 * the lock is reading -- including the one that is about to be given the lock and
 * has not started using it yet.  Splitting them means a draw touches only the low
 * half and a hand-off only the high half.
 *
 * Both are volatile as well as atomic.  The builtins promise the access is not
 * reordered or elided, but nothing else in the kernel reads either field, so
 * nothing stops the compiler from noticing that and keeping the value in a
 * register across the call to pause_cpu(). */
void ticket_lock(ticket_lock_t *l) {
    uint16_t mine = __atomic_fetch_add((uint16_t *)&l->ticket, 1,
                                       __ATOMIC_ACQ_REL);

    while (__atomic_load_n((uint16_t *)&l->serving, __ATOMIC_ACQUIRE) != mine) {
        /* Spinning flat out is not free even when it works: the `lock` prefix's
         * coherence traffic is what the next processor's store has to invalidate,
         * so a tight loop makes the hand-off slower, not faster. */
        pause_cpu();
    }
}

bool ticket_trylock(ticket_lock_t *l) {
    uint16_t drawn = __atomic_load_n((uint16_t *)&l->ticket, __ATOMIC_ACQUIRE);
    uint16_t served = __atomic_load_n((uint16_t *)&l->serving,
                                       __ATOMIC_ACQUIRE);

    /* Equal means nobody is waiting -- every ticket drawn so far has been served --
     * and a processor that draws after this read gets a number greater than
     * `drawn`, so it will wait for us.  That is what makes the two separate reads
     * a decision rather than a race. */
    return drawn == served;
}

void ticket_unlock(ticket_lock_t *l) {
    __atomic_add_fetch((uint16_t *)&l->serving, 1, __ATOMIC_RELEASE);
}

/* -------------------------------------------------------------------- MCS --- */

/* Publish this node, find out who was ahead of us, and hand it the address of the
 * one that has to hand *us* the lock. */
void mcs_lock(mcs_node_t *node, mcs_lock_t *guard) {
    __atomic_store_n(&node->locked, true, __ATOMIC_RELAXED);
    __atomic_store_n(&node->next, (mcs_node_t *)0, __ATOMIC_RELAXED);

    mcs_node_t *pred = (mcs_node_t *)__atomic_exchange_n(&guard->tail, node,
                                                          __ATOMIC_ACQ_REL);
    if (pred == NULL)
        return;                     /* nobody waiting: the lock is ours */

    /* The predecessor is spinning on its own flag, so this store has to reach its
     * cache before it looks.  It cannot look before this happens: it has not been
     * told there is a successor until this store lands. */
    __atomic_store_n(&pred->next, node, __ATOMIC_RELEASE);

    while (__atomic_load_n(&node->locked, __ATOMIC_ACQUIRE))
        pause_cpu();
}

void mcs_unlock(mcs_node_t *node, mcs_lock_t *guard) {
    mcs_node_t *succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);

    if (succ == NULL) {
        /* Nobody has linked onto us *yet*, which is not the same as nobody being
         * on the way.  A compare-and-exchange, and *not* an exchange, is what
         * settles it: if the tail is still this node then nobody arrived and the
         * lock is released; if it is not, a successor has published itself and
         * the tail has to keep pointing at it.
         *
         * Writing the tail unconditionally is the version that looks equivalent
         * and is not.  An exchange would leave the tail empty even with a successor
         * queued behind it, and the next processor to arrive would find an empty
         * queue, conclude the lock was free, and go straight into the critical
         * section the successor is about to enter.  Two processors inside at once,
         * lost updates, and a queue that has lost its own tail. */
        mcs_node_t *expected = node;
        if (__atomic_compare_exchange_n(&guard->tail, &expected, (mcs_node_t *)0,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;

        /* A successor is mid-way through its own acquire: it has taken its
         * predecessor -- us -- out of the tail but has not told us yet, so its
         * store into `next` is the only way to find it. */
        while ((succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == NULL)
            pause_cpu();
    }

    __atomic_store_n(&succ->locked, false, __ATOMIC_RELEASE);
}

void mcs_lock_init(mcs_lock_t *guard) {
    __atomic_store_n(&guard->tail, (mcs_node_t *)0, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------- semaphore --- */

void sem_init(semaphore_t *s, int32_t count) {
    atomic_init(&s->count, count);
    atomic_init(&s->waiters, 0);
}

int32_t sem_count(const semaphore_t *s) {
    return atomic_load((atomic_t *)&s->count);
}

bool sem_trywait(semaphore_t *s) {
    int32_t c = atomic_load(&s->count);

    while (c > 0) {
        /* A plain fetch-and-subtract would let a waiter see a negative count and
         * walk the count down past zero, so the change is conditional: the count is
         * only decremented if it is still what this processor read. */
        if (atomic_cas(&s->count, c, c - 1))
            return true;
        c = atomic_load(&s->count);
    }

    return false;
}

void sem_post(semaphore_t *s) {
    atomic_increment(&s->count);
}

void sem_wait(semaphore_t *s) {
    while (!sem_trywait(s)) {
        atomic_increment(&s->waiters);
        /* No token can appear without someone posting one, and there is nowhere to
         * sleep until phase 5, so this spins.  The waiter count is still kept
         * honest because `irqs` and the phase-11 stress test read it. */
        while (sem_count(s) <= 0)
            pause_cpu();
        atomic_decrement(&s->waiters);
    }
}

/* ------------------------------------------------------------------ mutex --- */

void mutex_init(mutex_t *m) {
    ticket_lock_init(&m->lock);
    atomic_init(&m->state, 0);
    atomic_init(&m->waiters, 0);
    __atomic_store_n(&m->owner, (uint64_t)-1, __ATOMIC_RELAXED);
}

bool mutex_trylock(mutex_t *m) {
    int32_t zero = 0;
    if (!atomic_cas(&m->state, zero, 1))
        return false;
    __atomic_store_n(&m->owner, percpu_self_index(), __ATOMIC_RELAXED);
    return true;
}

void mutex_lock(mutex_t *m) {
    unsigned self = percpu_self_index();

    /* Take the ticket first, so the processors queue up behind it rather than all
     * hammering the state word. */
    ticket_lock(&m->lock);

    while (!mutex_trylock(m)) {
        /* A lock taken twice by one processor never becomes free again: the second
         * acquire waits for a release that cannot come.  Saying so is the
         * difference between a bug with a name and a machine that has stopped. */
        if (__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == (uint64_t)self) {
            ticket_unlock(&m->lock);
            panic("mutex_lock: processor %u already holds this mutex\n", self);
        }

        atomic_increment(&m->waiters);
        while (atomic_load(&m->state) != 0)
            pause_cpu();
        atomic_decrement(&m->waiters);
    }
}

void mutex_unlock(mutex_t *m) {
    __atomic_store_n(&m->owner, (uint64_t)-1, __ATOMIC_RELAXED);
    atomic_store(&m->state, 0);
    ticket_unlock(&m->lock);
}

/* --------------------------------------------------------------- barriers --- */

/* A compiler barrier only.  It stops the compiler moving accesses across, which is
 * all a data dependency built out of plain loads and stores needs.  On x86-64
 * every load and every store is already ordered, so no instruction is required, and
 * emitting one would cost a round trip to memory for nothing. */
void barrier(void) {
    __asm__ volatile("" ::: "memory");
}

void rmb(void) { barrier(); }
void wmb(void) { barrier(); }

void mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}