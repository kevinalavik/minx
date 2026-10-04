/* minx - locking.
 *
 * Five kinds.  The difference between them is not what is protected but what the
 * holder is allowed to do while it has it:
 *
 *   spinlock  - busy-waits, may spin with interrupts on, never sleeps.  For
 *               anything that must not be preempted, and for anything taken in
 *               interrupt context.
 *   irqsave   - a spinlock that also turns interrupts off.  For anything a
 *               device can interrupt while it is held: without this, the handler
 *               runs on the same CPU mid-update and re-enters the lock.
 *   mutex     - for anything longer than a few instructions.  Does not disable
 *               interrupts, so it cannot protect against a handler either; take it
 *               from the handler with the _irqsave form.
 *   semaphore - counts, for producer/consumer and for "how many waiters".
 *
 * The mutex and the semaphore spin rather than sleep, and that is the one thing
 * here that is not finished: they give up mutual exclusion but not the processor.
 * They are correct, and they are the right lock for a short critical section; they
 * are the wrong lock for one that can block, which is why the wait queues they
 * would need are not here yet either.  Lock order is in docs/LOCKING.md.
 */
#ifndef MINX_SPINLOCK_H
#define MINX_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "boot.h"   /* MAX_CPU_COUNT */
#include "io.h"

/* ---------------------------------------------------------------- atomics --- */

/* Every one of these is a single instruction where x86-64 has one, and takes and
 * releases a full barrier.  A lock-free loop that looks right but is not is the
 * kind of bug that only shows up on four CPUs, so there is no non-barrier variant
 * to get wrong. */
typedef struct {
    int32_t counter;
} atomic_t;

void     atomic_init(atomic_t *a, int32_t value);
int32_t  atomic_load(const atomic_t *a);
void     atomic_store(atomic_t *a, int32_t value);
int32_t  atomic_fetch_add(atomic_t *a, int32_t delta);
int32_t  atomic_fetch_sub(atomic_t *a, int32_t delta);
/* Returns the value *before* the exchange, like every other atomic read-modify
 * write.  CAS is a loop rather than a compare-and-swap because the instruction
 * x86-64 offers is `lock cmpxchg`, which only compares and swaps. */
bool     atomic_cas(atomic_t *a, int32_t expected, int32_t desired);
int32_t  atomic_increment(atomic_t *a);
int32_t  atomic_decrement(atomic_t *a);
bool     atomic_trylock(atomic_t *a);

/* The processor-local flag the kernel uses for a per-CPU "nobody else may touch
 * this" claim.  `lock inc` on a byte in memory rather than a register, because a
 * register has no per-CPU meaning and the whole point is that only this processor
 * ever looks at it. */
void     cpu_lock_acquire(void);
void     cpu_lock_release(void);
bool     cpu_lock_held(void);

/* ----------------------------------------------------------- ticket locks --- */

/* The lock word is one 32-bit value with two 16-bit counters packed into it: the
 * number the lock is serving, and the number the caller drew.  A holder is
 * `serving == mine`, and a waiter waits for `serving == mine` rather than for the
 * word to become zero.
 *
 * That is what makes it fair.  A test-and-set spinlock hands the lock to
 * whichever waiting processor's store to memory lands first, which on a contended
 * lock is close to random and under TCG is often the same one CPU over and over --
 * so the others can be starved indefinitely while the winner goes round after
 * round.  With tickets, `serving` only ever moves to the next number drawn, so
 * every waiter gets it in turn.  The cost is one extra 16-bit fetch to release.
 */
typedef struct {
    uint32_t ticket;
    uint32_t serving;
} ticket_lock_t;

void ticket_lock_init(ticket_lock_t *l);
void ticket_lock(ticket_lock_t *l);
bool ticket_trylock(ticket_lock_t *l);
void ticket_unlock(ticket_lock_t *l);

/* An MCS lock: each waiter holds its own queue node on its own stack, so the
 * queue is a linked list of stack addresses rather than a shared tail that every
 * waiter has to compare-and-swap.  Unlike the ticket lock it does not have to
 * hand the lock out in ticket order, so a processor that arrives while the lock
 * is held goes to the back of the queue and the holder's successor is the one that
 * was already waiting -- which keeps the waiter's own cache line hot instead of
 * everyone hammering one word. */
typedef struct mcs_node {
    struct mcs_node *next;
    volatile bool    locked;
} mcs_node_t;

/* The queue head.  Only the processor that has the lock touches it, and only to
 * exchange a pointer, so the coherence traffic is one line per hand-off rather
 * than one per waiter. */
typedef struct {
    mcs_node_t *tail;
} mcs_lock_t;

void mcs_lock_init(mcs_lock_t *guard);
void mcs_lock(mcs_node_t *node, mcs_lock_t *guard);
void mcs_unlock(mcs_node_t *node, mcs_lock_t *guard);

/* ---------------------------------------------------------- interrupt-safe --- */

/* Interrupts off, then spin.  For anything a device interrupt can touch. */
static inline uint64_t spin_lock_irqsave(ticket_lock_t *l) {
    uint64_t flags = irq_save();
    ticket_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(ticket_lock_t *l,
                                          uint64_t flags) {
    ticket_unlock(l);
    irq_restore(flags);
}

/* `flags` is deliberately passed back: restoring the *saved* state rather than
 * unconditionally enabling interrupts is what makes it safe to take a lock that
 * was already held with interrupts off, which is what a handler does. */

static inline bool spin_trylock_irqsave(ticket_lock_t *l) {
    irq_save();
    return ticket_trylock(l);
}

/* ------------------------------------------------------------- semaphore --- */

typedef struct {
    atomic_t count;
    atomic_t waiters;
} semaphore_t;

typedef semaphore_t sem_t;

void sem_init(semaphore_t *s, int32_t count);
bool sem_trywait(semaphore_t *s);
void sem_wait(semaphore_t *s);
void sem_post(semaphore_t *s);
int32_t sem_count(const semaphore_t *s);

/* ------------------------------------------------------------------ mutex --- */

typedef struct {
    ticket_lock_t lock;
    atomic_t      state;      /* 0 free, 1 held, 2 held with waiters */
    atomic_t      waiters;
    /* The owning processor, so a self-deadlock is reported rather than hanging. */
    uint64_t      owner;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* ----------------------------------------------------------- wait queues --- */

/* Deliberately absent.  A wait queue needs somewhere to put a waiter that is not
 * burning a processor, and that is the scheduler: phase 5.  A queue that spins
 * until woken would be a wait queue in name only -- it would hold a processor's
 * whole time slice instead of giving it back, and the moment a critical section
 * could block it would be the wrong primitive, which is the only situation a wait
 * queue exists for.  The semaphore and the mutex above are honest about the same
 * limit.
 */

/* --------------------------------------------------------------- barriers --- */

/* The compiler cannot reorder across these; on x86-64 each is the fence
 * instruction and nothing more.  `rmb`/`wmb` are free (loads and stores are
 * already ordered), so they are a compiler barrier and nothing else. */
void barrier(void);
void mb(void);      /* full fence */
void rmb(void);
void wmb(void);

#endif /* MINX_SPINLOCK_H */