/* minx - kernel tasks, per-processor run queues and the scheduler.
 *
 * The interface is kernel/include/task.h.  What is worth stating here is the
 * locking rule, because it is the whole difference between this and the version
 * with one lock around everything:
 *
 *   A run queue lock is taken, the queue is examined, and the lock is dropped
 *   before any context switch happens.  A switch itself takes no locks at all.
 *   So there is no lock held across a switch and none held across anything that
 *   could block, and the ordering between two run queues is never a question --
 *   there is no ordering because there is only ever one lock in hand.
 *
 * The consequence that costs something: a task chosen for a processor can be
 * taken by another processor between the moment the queue lock is dropped and the
 * moment it is switched to.  `state` is the arbiter -- only one processor ever
 * wins the transition from TASK_RUNNABLE to TASK_RUNNING, and the loser simply
 * gives the task back.
 */
#include "signal.h"
#include "task.h"

#include "apic.h"
#include "boot.h"
#include "cpu.h"
#include "delay.h"
#include "fd.h"
#include "procfs.h"
#include "ramfs.h"
#include "io.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "serial.h"
#include "smp.h"
#include "string.h"
#include "timer.h"
#include "vectors.h"
#include "vmm.h"





/* Provided by kernel/arch/x86_64/switch.S.  task_start_shim is jumped to by the
 * `ret` at the end of a first switch onto a task; it reads the entry point and
 * the argument off that task's stack and calls task_start(). */
/* task_switch() is the one-instruction-pair primitive: it saves and switches as a
 * single indivisible step, which is only correct when the outgoing task is not going
 * to be published to another processor before the switch.  schedule() has to do the
 * two halves separately, for the reason given at task_save_and_publish() below, so
 * those are separate entry points here too.
 *
 * task_switch() is retained: it remains the correct primitive for a switch that cannot
 * race, and nothing in the tree has been removed to make room for the split. */
void task_switch(uint64_t *save_rsp, uint64_t new_rsp);
void task_restore_context(uint64_t new_rsp);
void task_enter_first(uint64_t new_rsp);
void task_start_shim(void);
void task_start(void (*entry)(void *), void *arg);
void task_enter_user(void);

/* The idle task is a task like any other, and the scheduler reaches it through
 * the same path. */
static void idle_entry(void *arg);

/* Every task, for the self tests and `ps`.  The walk is under the table's lock,
 * so a task created while it is being walked cannot be missed halfway. */
unsigned task_count(void);
task_t  *task_by_index(unsigned i);

/* ------------------------------------------------------------ per-CPU ---- */

typedef struct {
    ticket_lock_t lock;
    task_t       *head;
    task_t       *tail;
    uint32_t      length;
} run_queue_t;

typedef struct {
    run_queue_t   rq;
    task_t       *current;
    task_t       *idle;
    volatile uint32_t need_resched;
    volatile uint32_t in_schedule;   /* re-entrancy guard, per processor */
    uint64_t      slice_start_ns;
    uint64_t      ticks;
    uint32_t      switches;
    uint32_t      steals;
    uint32_t      migrations;
    uint32_t      idle_ticks;
    uint32_t      claim_failed_reported;
    uint32_t      self_returned_reported;
    uint32_t      reentry_reported;
} sched_cpu_t;

/* Provided by kernel/arch/x86_64/switch.S, and the reason schedule() cannot be a
 * single call to task_switch(): it saves the outgoing task's context and publishes it
 * in one indivisible step, because a task that another processor can reach before its
 * saved stack pointer is final runs a stack frame that is already in use.  Declared
 * here because it takes a sched_cpu_t. */
void task_save_publish_and_switch(uint64_t *save_rsp, task_t *prev, sched_cpu_t *c,
                                 uint64_t next_rsp,
                                 const page_table_t *next_tables,
                                 int32_t *prev_saved_in_user,
                                 int32_t *next_saved_in_user,
                                 int32_t from_user,
                                 const void *out_ctx);

static sched_cpu_t cpu[MAX_CPU_COUNT];

/* Tasks that are not runnable: asleep until a time, or blocked on a wait queue.
 * One list for both, because both are woken by walking it, and its own lock for
 * the same reason the run queues have one -- a wake from an interrupt must not
 * have to reach into another processor's queue. */
static struct {
    ticket_lock_t lock;
    task_t       *head;
    task_t       *tail;
    uint32_t      length;
} blocked;

/* Every task ever created, so the self tests can walk them.  Created under a
 * lock, because a task can be created from any processor. */
static struct {
    ticket_lock_t lock;
    task_t       *head;
    task_t       *tail;
    uint32_t      count;
} all_tasks;

static uint64_t next_task_id = 1;

/* Whether sched_init() has finished.
 *
 * The boot processor's local APIC timer is armed before the scheduler exists --
 * it is armed long before anything else, because it is what proves the interrupt
 * path works.  So a tick can arrive while there are no run queues and no idle
 * task, and a tick that tried to schedule then would have nothing to schedule.
 * The flag is what the tick checks, and it is set once, last, in sched_init(). */
static volatile uint32_t sched_up;


/* How long one time slice lasts.  Short enough that a spinning task cannot hold
 * a processor, long enough that the switch is not the interesting cost. */
#define SCHED_SLICE_NS  2000000ull    /* 2 ms */

/* A task's stack, for the same reasons and the same size as SMP_STACK_SIZE above:
 * it carries kernel code, the scheduler, and an interrupt frame on top of whatever
 * the task itself has on it. */
#define TASK_STACK_SIZE 32768

/* How many tasks can exist.  The per-CPU table has a room for this many, and the
 * bound is here so that "one task linked into two lists" fails at the point of
 * the mistake rather than as a queue that slowly stops working. */
#define TASK_MAX 1024

/* ---------------------------------------------------------- list helpers -- */

static void list_link(task_t **head, task_t **tail, task_t *t) {
    if (t->list_tag != 0)
        panic("smp: task %lu (%s) is already linked into the list at %p and is "
              "being linked into that same list again (state %d, entry %p)\n",
              (unsigned long)t->id, t->name, (void *)(uintptr_t)t->list_tag,
              t->state, (void *)(uintptr_t)t->entry);
    t->list_tag = (uint64_t)(uintptr_t)head;
    t->prev = *tail;
    t->next = NULL;
    if (*tail != NULL)
        (*tail)->next = t;
    else
        *head = t;
    *tail = t;
}

static void list_unlink(task_t **head, task_t **tail, task_t *t) {
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        *head = t->next;
    if (t->next != NULL)
        t->next->prev = t->prev;
    else
        *tail = t->prev;
    t->next = NULL;
    t->prev = NULL;
    t->list_tag = 0;
}

/* ------------------------------------------------------------- run queue --- */

static void rq_push(sched_cpu_t *c, task_t *t) {
    /* Two invariants, both checked, because a run queue that is quietly wrong does
     * not fail: it hands the same task to two processors or loses one, and both
     * look like a scheduler that has stopped making progress. */
    if (t->list_tag != 0)
        panic("smp: task %lu (%s) is already linked into the list at %p, and "
              "is being queued on processor %u as well (state %d, "
              "queued there %u, queued here %u)\n", (unsigned long)t->id,
              t->name, (void *)(uintptr_t)t->list_tag, percpu_self_index(),
              t->state, (unsigned)t->list_tag,
              (unsigned)c->rq.length);
    if (c->rq.length >= TASK_MAX)
        panic("smp: run queue for processor %u is full (%u tasks)\n",
              percpu_self_index(), c->rq.length);

    list_link(&c->rq.head, &c->rq.tail, t);
    c->rq.length++;
}

static void rq_remove(sched_cpu_t *c, task_t *t) {
    if (c->rq.length == 0)
        panic("smp: removing task %lu (%s) from an empty run queue\n",
              (unsigned long)t->id, t->name);
    if (t->list_tag != (uint64_t)(uintptr_t)&c->rq.head)
        panic("smp: task %lu (%s) is being removed from processor %u's run "
              "queue, but it is linked into the list at %p\n",
              (unsigned long)t->id, t->name, percpu_self_index(),
              (void *)(uintptr_t)t->list_tag);

    list_unlink(&c->rq.head, &c->rq.tail, t);
    c->rq.length--;
}

/* The runnable task with the least virtual runtime.
 *
 * Called with the queue's lock held.  A tie goes to the earlier task in the
 * queue, which is what stops two equal-weight tasks from taking turns in lock
 * step forever: each is chosen immediately after the other and neither ever sees
 * the other's work finish. */
/* Walking a list of tasks is trusting every link in it, including the head.
 *
 * The head is the one that is easiest to get wrong and the hardest to notice, because
 * a walk that starts on a node which is not on the list looks exactly like a walk that
 * starts on a task with an odd state: the node's `state`, `weight` and `vruntime` are
 * all read, all of them mean something, and none of them is a pointer.  That is how a
 * heap address reaches `current` -- not by being *followed* to, but by already being
 * the first thing the walk looks at.
 *
 * The check costs a compare against a value already in cache, and it turns "the list
 * head is not a task on this list" into a report that names it. */
static task_t *list_head_checked(task_t *head, task_t **list_head,
                                 const char *what) {
    if (head != NULL && head->list_tag != (uint64_t)(uintptr_t)list_head)
        panic("smp: the %s starts at task %p, which is on the list at %p, not "
              "this one\n", what, (void *)head,
              (void *)(uintptr_t)head->list_tag);

    return head;
}

static task_t *list_next_checked(task_t *t, task_t **head, const char *what) {
    task_t *next = t->next;

    if (next != NULL && next->list_tag != (uint64_t)(uintptr_t)head)
        panic("smp: walking the %s, task %lu (%s) points at task %p, which is on "
              "the list at %p\n", what, (unsigned long)t->id, t->name,
              (void *)next, (void *)(uintptr_t)next->list_tag);

    return next;
}

static task_t *rq_pick_fairest(sched_cpu_t *c) {
    task_t *best = NULL;
    task_t *t = list_head_checked(c->rq.head, &c->rq.head, "run queue for "
                                  "this processor");

    while (t != NULL) {
        task_t *next = list_next_checked(t, &c->rq.head, "run queue");

        if (t->state == TASK_RUNNABLE &&
            (best == NULL || t->vruntime < best->vruntime))
            best = t;
        t = next;
    }

    return best;
}

static task_t *rq_pop(sched_cpu_t *c) {
    task_t *t = rq_pick_fairest(c);

    if (t != NULL)
        rq_remove(c, t);
    return t;
}

/* Enqueue and, if it went to another processor, wake that processor. */
static void enqueue(unsigned index, task_t *t) {
    if (index >= MAX_CPU_COUNT || t == NULL)
        return;

    sched_cpu_t *c = &cpu[index];

    ticket_lock(&c->rq.lock);
    rq_push(c, t);
    ticket_unlock(&c->rq.lock);

    /* A processor that is asleep does not look at its run queue, and an IPI is
     * what wakes it.  The reschedule handler then runs what is queued.  Skipped
     * for this processor: it is already awake and about to look. */
    if (index != percpu_self_index())
        smp_kick_cpu(index);
}

/* --------------------------------------------------------- blocked list --- */

static void blocked_add(task_t *t) {
    list_link(&blocked.head, &blocked.tail, t);
    blocked.length++;
}

/* Removing a task that is not on this list is the quiet kind of wrong.
 *
 * `list_unlink()` trusts its arguments: it patches the node's neighbours and writes
 * the node's own `next`/`prev` back as the list's new head and tail.  Handed a task
 * whose links are already clear, it therefore sets *both* of them to NULL -- which
 * does not fail, it throws the entire list away, and every sleeper still on it is
 * now unreachable and will never be woken.  The visible result is a task that slept
 * and never came back, which reads like a lost timer rather than like a lost list.
 *
 * So the node is asked which list it is on first.  This was the one unlinked list
 * without the check, and it is what a corrupted `current` most plausibly follows
 * from. */
static void blocked_remove(task_t *t) {
    if (t->list_tag != (uint64_t)(uintptr_t)&blocked.head)
        panic("smp: task %lu (%s) is being taken off the blocked list, but it is "
              "on the list at %p, not this one\n", (unsigned long)t->id, t->name,
              (void *)(uintptr_t)t->list_tag);

    list_unlink(&blocked.head, &blocked.tail, t);
    blocked.length--;
}

/* Put a blocked task back on some run queue.
 *
 * Two things have to happen together here, and the second is the one that is easy
 * to miss: the task has to come *off* the blocked list as well as onto a run
 * queue.  Leaving it on the blocked list means it is in two lists, and the next
 * unlink from either of them splices the other one -- so the run queue it was put
 * on stops being traversable, and a task that later blocks again is linked into it
 * a second time.  Both symptoms look like a scheduler that has stopped making
 * progress rather than like memory corruption.
 *
 * The transition is done under the blocked list's lock, so a task woken twice --
 * or woken while a timed sleep is also expiring -- is only unlinked once. */
static void unblock(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&blocked.lock);

    int32_t expected = TASK_WAITING;
    bool was_waiting = __atomic_compare_exchange_n(&t->state, &expected,
                                                   TASK_RUNNABLE, false,
                                                   __ATOMIC_ACQ_REL,
                                                   __ATOMIC_ACQUIRE);
    if (was_waiting)
        blocked_remove(t);

    spin_unlock_irqrestore(&blocked.lock, flags);

    /* Already runnable, or already queued: a second wake, or a timed sleep that
     * fired first.  Either way there is nothing left to do. */
    if (!was_waiting)
        return;

    t->wake_at_ns = 0;
    enqueue(percpu_self_index(), t);
}

/* Forget that there is a ring-3 context on this processor's stack.
 *
 * Two paths need this, and they are the two that finish with the exception handler's C
 * code instead of passing through it.
 *
 * The syscall door clears it on its way out, because leaving the marker set would hand the
 * *next* save a ring-3 frame that is not there: the task would resume through `iretq` off
 * whatever the handler had left on its stack.
 *
 * A task that is stopping in the middle of a syscall clears it for the same reason and
 * with more at stake.  A ring-3 task is normally saved whole -- the interrupt frame the
 * CPU pushed -- and resumed through `iretq`, which is the only way back to ring 3 with the
 * right CS, SS and %rsp.  That is right for a task that was interrupted and is simply
 * waiting its turn.  It is wrong for one blocked in a syscall: the frame's RIP is the
 * instruction after `int $0x80`, so resuming it delivers the syscall's return value into
 * user code -- whatever is in %rax when the task stopped, which is the call *number*,
 * because nothing has computed a result yet.  A program then carries on believing its read
 * returned 10, and the kernel's half of the buffer it was filling in is still waiting for
 * whoever was supposed to be woken.
 *
 * Clearing it before the switch means such a task is saved the ordinary way: the
 * callee-saved registers and a return address, with its C frames still on its kernel
 * stack, intact.  Woken, it returns from `schedule()`, unwinds back through the syscall,
 * computes the answer, writes it into the interrupt frame, and that frame's own `iretq` --
 * still waiting where the CPU left it -- returns to ring 3 properly.  The answer is not
 * lost either way: it is the same frame.
 *
 * `percpu_t.from_user` is consumed by exactly one reader, schedule(), so clearing it here
 * cannot affect a save this task is not part of. */
static void self_from_user_clear(void) {
    percpu_self()->from_user = 0;
    percpu_self()->user_frame = NULL;
}

/* The exported half of unblock(): see the header for why waking a blocked task is not
 * the same operation as queueing a runnable one. */
void task_unblock(task_t *t) {
    unblock(t);
}

/* ------------------------------------------------------------- creation --- */

static task_t *task_alloc(void) {
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);

    if (all_tasks.count >= TASK_MAX) {
        spin_unlock_irqrestore(&all_tasks.lock, flags);
        return NULL;
    }

    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    spin_unlock_irqrestore(&all_tasks.lock, flags);

    if (t == NULL)
        return NULL;

    memset(t, 0, sizeof(*t));
    t->state = TASK_UNUSED;
    t->weight = TASK_WEIGHT_DEFAULT;
    t->last_cpu = MAX_CPU_COUNT;

    /* The three descriptors every process starts with, filled in here rather than by each
     * creator: a task that reached the point of being switched into with an empty
     * descriptor table would have no stdout, and the symptom would be a program whose
     * output silently goes nowhere. */
    for (int i = 0; i < 3; i++) {
        t->fds[i].kind = FD_CONSOLE;
        t->fds[i].closable = false;
    }

    /* Root, and the root of the filesystem as a node.
     *
     * Both are set here rather than by whoever creates the task, because a task that
     * reached the point of being switched into without them has a current directory of
     * nothing, and every relative path it uses fails -- which reads as "the kernel has no
     * files" rather than as "the task has no directory". */
    strlcpy(t->cwd, "/", sizeof(t->cwd));
    t->cwd_ino = ramfs_root();
    t->cwd_mount = 0;
    t->uid = 0;
    t->gid = 0;
    t->umask = 0022;
    t->session = 0;
    t->pgrp = 0;

    return t;
}

/* The list of every task, in creation order.  Singly linked through `all_next`,
 * because a task stays in it for ever and the transient links are needed for the
 * run queues. */
static void task_register(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);

    t->id = next_task_id++;

    task_t **tailp = &all_tasks.head;
    while (*tailp != NULL)
        tailp = &(*tailp)->all_next;
    *tailp = t;
    t->all_next = NULL;
    all_tasks.tail = t;
    all_tasks.count++;

    spin_unlock_irqrestore(&all_tasks.lock, flags);

    /* The `/proc` entry, here and not at the call sites.
     *
     * This is the one point where a task stops being anonymous: it has an identifier and
     * it is on the list everything walks.  `ps` starts listing it here, so `/proc` has to
     * as well, or the two disagree about which processes exist -- and `ps` reading a
     * process that `/proc` does not have is a program that cannot report anything about
     * it.
     *
     * It was at the call sites instead, in `task_create_forked()` only, and that is not a
     * narrower rule than it sounds: it meant *children* had a directory and programs the
     * kernel started did not.  `fstest` asked for `/proc/self/status` and was told there
     * was no such file, on a machine with four processors and fifty-odd processes in it.
     *
     * After the all-tasks lock, deliberately: `procfs_note_task()` allocates, and the
     * order that is safe is the one where a task is fully published before anything more
     * expensive is done on its behalf. */
    procfs_note_task(t);
}

/* Lay a fresh kernel stack out for task_switch's pops, and for the `ret` at the
 * end of them to land somewhere useful.
 *
 * The pops take six quadwords -- r15 down to rbp -- and the `ret` takes the
 * seventh, which has to be an address to jump to.  Above that are the entry point
 * and its argument, which the shim at that address picks up.
 *
 *   frame[0..5]  r15 r14 r13 r12 rbx rbp
 *   frame[6]     where `ret` goes: the bootstrap shim
 *   frame[7]     the task's entry point
 *   frame[8]     its argument
 */
static bool task_stack_init(task_t *t, void (*entry)(void *), void *arg) {
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);

    if (stack == NULL)
        return false;

    uint64_t *frame = (uint64_t *)(((uint64_t)stack + TASK_STACK_SIZE) & ~15ull);

    frame -= 9;
    for (unsigned i = 0; i <= 5; i++)
        frame[i] = 0;
    frame[6] = (uint64_t)(uintptr_t)task_start_shim;
    frame[7] = (uint64_t)(uintptr_t)entry;
    frame[8] = (uint64_t)(uintptr_t)arg;

    /* The guard goes at the very bottom, so the first byte written past the stack
     * is the one that reports it. */
    *(volatile uint64_t *)stack = TASK_STACK_GUARD;
    t->stack_guard = (volatile uint64_t *)stack;
    t->stack_low = (uint64_t)stack;

    t->stack = stack;
    t->stack_top = (uint64_t)stack + TASK_STACK_SIZE;
    t->saved_rsp = (uint64_t)frame;
    t->entry = entry;
    t->arg = arg;
    return true;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg,
                    uint32_t weight) {
    task_t *t = task_alloc();

    if (t == NULL)
        return NULL;

    strlcpy(t->name, name, sizeof(t->name));
    t->weight = weight < TASK_WEIGHT_MIN ? TASK_WEIGHT_MIN
           : (weight > TASK_WEIGHT_MAX ? TASK_WEIGHT_MAX : weight);
    t->state = TASK_NEW;

    if (!task_stack_init(t, entry, arg)) {
        kfree(t);
        return NULL;
    }

    task_register(t);
    return t;
}

/* Take a task off the all-tasks list.
 *
 * Only for a task that is being *thrown away*, not one that has finished: a task that has
 * exited stays on the list, because a parent blocked in `wait` is looking for it there and
 * `ps` wants to show it.  This is for the other case, where the task was built only to
 * have something built in and is about to be freed.
 *
 * The pairing is the point.  `task_register()` puts a task on this list and gives it an
 * identifier; anything that frees a `task_t` has to call this first, because a list entry
 * pointing at freed memory is a dangling pointer that every walk of the list -- `ps`,
 * `wait`, `task_by_pid()`, `/proc/<pid>` -- follows into whatever the allocator handed
 * that block out as next.  `program_exec_into()` built a scratch task to load a program
 * into and freed it without this, and the symptom was `/proc` reporting a task whose
 * fields were whatever a later allocation had put there. */
void task_unregister(task_t *t) {
    if (t == NULL)
        return;

    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);
    task_t **linkp = &all_tasks.head;

    while (*linkp != NULL && *linkp != t)
        linkp = &(*linkp)->all_next;

    if (*linkp == t) {
        *linkp = t->all_next;
        all_tasks.count--;

        /* The tail moves too, and to the task *before* the one removed -- which is not
         * the one that followed it.  Getting this wrong leaves `all_tasks.tail` pointing
         * at the task that just left, and every later registration walks the list again
         * from the head anyway, so the list would look right while the tail was not. */
        if (all_tasks.tail == t) {
            task_t *last = NULL;

            for (task_t *w = all_tasks.head; w != NULL; w = w->all_next)
                last = w;
            all_tasks.tail = last;
        }
    }

    t->all_next = NULL;
    spin_unlock_irqrestore(&all_tasks.lock, flags);

    /* And out of `/proc`, which the list alone does not cover.  A `/proc` entry naming a
     * task that no longer exists is a program that reads a name and gets nothing. */
    procfs_forget_task(t);
}

/* Lay out a user task's stack with a return frame that enters ring 3.
 *
 * The shape is the one the CPU itself makes when it takes an interrupt from ring 3:
 * RFLAGS, CS, RIP, then SS and RSP, because `sysretq` and `iretq` both pop them in
 * that order and there is no way to enter ring 3 with fewer.  Starting a task by
 * building that frame and returning into it means one entry path serves both kernel
 * tasks (through task_start_shim) and user tasks, and it means a user task's very
 * first instruction is ordinary user code.
 *
 * RFLAGS has IF set, so the task's first interrupt is its own choice rather than
 * something it inherited from the kernel that created it. */
task_t *task_create_user(const char *name, uint64_t entry, uint32_t stack_pages,
                         uint32_t weight) {
    /* A ring-3 task must start at an address ring 3 can reach.  Zero is allowed as a
     * placeholder, and only zero: the loader knows the entry point only after the task
     * exists, because loading needs the address space the task owns.  Zero is safe as a
     * placeholder because it is not a reachable address -- the first page of user space
     * is deliberately never mapped -- so a program that somehow started there would take
     * a page fault and say so, rather than running whatever happened to be at zero. */
    if (entry != 0 && !vmm_is_user_addr(entry))
        return NULL;

    task_t *t = task_create(name, NULL, NULL, weight);

    if (t == NULL)
        return NULL;

    page_table_t *tables = kmalloc(sizeof(*tables));

    if (tables == NULL || vmm_address_space_create(tables) != 0) {
        if (tables != NULL)
            kfree(tables);
        kfree(t);
        return NULL;
    }

    t->tables = tables;
    t->is_user = true;

    /* The signal-return trampoline, in every user address space.
     *
     * It has to be mapped here rather than on first use because the first use is *inside*
     * delivery: a handler returning to an unmapped page is a #PF on the instruction after
     * the handler, with the handler's own work already done and nothing in the report
     * pointing at the signal.  Read-only and shared, so it is one copy in the machine. */
    if (signal_map_trampoline(tables) != 0) {
        kwarn("task: %s could not be given the signal trampoline", name);
        vmm_address_space_destroy(tables);
        kfree(tables);
        kfree(t);
        return NULL;
    }

    /* The stack: whole pages, allocated, zeroed, mapped user-writable at the top of
     * the user address space.  Mapped only in *this* address space -- a kernel thread
     * must not be able to reach it through the direct map, which is what CR4.SMAP is
     * for and why the user half lives in different PML4 entries from the kernel's. */
    uint32_t pages = stack_pages == 0 ? 1 : stack_pages;
    uint64_t top = USER_STACK_TOP;
    uint64_t top_phys = 0;

    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = vmm_user_page_alloc();

        /* The page at the very top is the one the two frames go in, and the kernel has
         * to be able to write them, so its physical address is kept: the frames are
         * reachable only through the direct map of that page. */
        if (i == 0)
            top_phys = phys;
        if (i + 1 == pages)
            t->user_stack_phys = phys;
        t->user_stack_pages = pages;

        if (phys == 0) {
            vmm_address_space_destroy(tables);
            kfree(tables);
            kfree(t);
            return NULL;
        }

        /* Into *this* task's address space, not the running one's.  vmm_map_user() maps
         * through vmm_tables(), which is the running task's tables, and a task being
         * created is not the running task -- so the stack went into the creator's address
         * space (the boot task's, whose tables are the kernel's), and the task was created
         * with a stack nothing could reach.  Its own tables had no entry for the address,
         * so the first access to its stack faulted while the walk over those same tables
         * correctly reported nothing mapped there.
         *
         * The same mistake is in the ELF loader's segment mapping; both are the reason
         * vmm_map_into() and vmm_translate_in() exist. */
        if (vmm_map_into(tables, top - (uint64_t)(i + 1) * page_size, phys,
                         page_size, true) != 0) {
            vmm_user_page_free(phys);
            vmm_address_space_destroy(tables);
            kfree(tables);
            kfree(t);
            return NULL;
        }
    }

    /* The two frames, laid out together at the very top of the stack.
     *
     * Reading upwards from `top - 96`, which is where `saved_rsp` points and therefore
     * the lowest address here:
     *
     *     top - 96   r15           \
     *     top - 88   r14            |  the scheduler's saved context: six
     *     top - 80   r13            |  callee-saved registers, then a return
     *     top - 72   r12            |  address, which the restore's `ret` takes.
     *     top - 64   rbx            |  They are in push order, so the pops read them
     *     top - 56   rbp           /   back the right way round.
     *     top - 48   task_enter_user
     *     top - 40   RIP            \
     *     top - 32   CS             |  the ring-3 return frame, in the order `iretq`
     *     top - 24   RFLAGS         |  pops it: RIP, CS, RFLAGS, then SS and RSP.
     *     top - 16   SS             |  Five quadwords, and the CPU enters ring 3 with
     *     top -  8   RSP           /   %rsp pointing at RIP, so task_enter_user() adds
     *                                  nothing before its `iretq`.
     *
     * The return address has to be a *kernel* address, because that is what runs when
     * the context is restored; it drops the stack to the ring-3 frame and hands it to
     * `iretq`.  The two layouts have to agree exactly, and the only way to keep them
     * agreeing is to write both from the same constants -- so USER_CTX_BYTES is the
     * distance from the saved context to the end of the ring-3 frame, and
     * task_enter_user() in switch.S adds precisely that much.
     *
     * Building this needs a kernel write into a user page, which CR4.SMAP would forbid.
     * SMAP is deliberately not enabled: it is one bit, but it turns every kernel write to
     * a user page from "works" into "faults", and there are exactly three such places
     * (here, the loader's segment copies and its stack) which would each then need an
     * stac/clac pair and a comment saying why.  Turning it on is a small change; leaving
     * it off is the honest default while the list is this short. */
    uint64_t *frame = (uint64_t *)(phys_to_virt(top_phys) + page_size
                                   - USER_FRAME_BYTES);
    uint64_t *ctx = (uint64_t *)(phys_to_virt(top_phys) + page_size
                                 - USER_FRAME_BYTES - USER_CTX_BYTES);
    uint64_t user_stack_top = top - (uint64_t)pages * page_size;

    /* In the order `iretq` pops them, each one quadword above the last, with %rsp left
     * pointing at RIP: RIP, CS, RFLAGS, RSP, SS.
     *
     * Note that RSP is *fourth* and SS is fifth, which is the easy thing to get wrong and
     * which this got wrong twice, in both directions at different times.  Reading a stack
     * pointer where a selector belongs raises #GP whose error code is the low half of that
     * stack pointer -- an error code that looks like a selector and is not one, and that
     * points at the frame rather than at anything the CPU did with it.  An earlier version
     * also had the frame itself the wrong way round (RFLAGS first, RIP last) and
     * compensated in task_enter_user() by advancing %rsp, which put RIP in the right slot
     * by luck and left CS holding the SS selector.  Two bugs, one symptom.
     *
     * The addressing is not negotiable either.  `virt_to_phys()` subtracts the direct-map
     * offset, so it is only meaningful for a kernel address; applied to a user address it
     * wraps, and `phys_to_virt()` of the result lands nowhere.  The frames are in a page
     * that exists only in this task's address space, so the direct map of that page's own
     * physical address is the one route to them. */
    frame[0] = entry;                      /* RIP */
    frame[1] = GDT_USER_CODE_RPL3;         /* CS: RPL 3, which is what makes iretq
                                           *   actually drop to ring 3 */
    /* IF set, so the task takes its own interrupts from its first instruction.
     *
     * This was 0x002, which is only the reserved bit that the CPU forces to one: IF is
     * bit 2, so 0x002 has interrupts *off*.  It read as though it were right, and leaving
     * it off meant a ring-3 task was never preempted -- which hid the whole class of
     * problem below, because a task that is never interrupted in ring 3 never has a
     * ring-3 context to save.  Turning it on is what makes this honest.
     *
     * The task's own DF and arithmetic flags are left clear, which is what the ABI
     * requires at a program's first instruction. */
    frame[2] = 0x202;                      /* RFLAGS: IF set, DF clear */
    frame[4] = GDT_USER_DATA_RPL3;         /* SS: fifth, after RSP */
    frame[3] = user_stack_top;             /* RSP: fourth, before SS */

    for (unsigned i = 0; i < 6; i++)
        ctx[i] = 0;
    ctx[6] = (uint64_t)(uintptr_t)task_enter_user;

    /* `saved_rsp` is what the CPU loads into RSP, so it is a *virtual* address in the
     * address space that will be running -- the same thing a kernel task's saved_rsp is,
     * since kmalloc() hands out direct-map addresses.  Storing the physical address here
     * would load a physical address into RSP, and with a 64 GiB heap under the direct map
     * that is an address in the middle of nothing in particular. */
    t->saved_rsp = top - USER_FRAME_BYTES - USER_CTX_BYTES;
    t->user_frame = frame;

    /* The kernel stack that task_create() already allocated is *kept*.
     *
     * It used to be thrown away here, on the grounds that a user task does not need one
     * yet.  It needs one immediately: `int $0x80` and every interrupt from ring 3 switch
     * to TSS.RSP0, and tss_init_for() zeroes the TSS, so RSP0 was zero on every processor
     * in the machine.  The consequence is that a user task's very first syscall pushes its
     * frame at address zero, and the handler that catches the resulting page fault runs on
     * an IST stack with the *original* registers -- so the fault is reported somewhere
     * entirely unrelated to the syscall that caused it, with a stack pointer that means
     * nothing.
     *
     * Keeping it also gives the task guarded, per-task kernel frames, which is what makes
     * a stack trace of a fault inside a syscall worth reading at all.
     *
     * `saved_rsp` still points into the *user* stack, because that is where the ring-3
     * return frame and the bootstrap context are, and entering the task for the first time
     * has to land between them.  After that it is a kernel-stack address like any other
     * task's, because that is where the CPU puts the frame.  Both are reachable in this
     * task's address space -- the kernel half is shared -- so nothing else has to change. */
    if (t->stack == NULL || t->stack_top == 0)
        return NULL;                      /* task_create() gave it one; check anyway */

    return t;
}

/* The idle task: what a processor runs when it has nothing else.  It halts rather
 * than spins, so an idle processor costs nothing and an incoming IPI is what
 * wakes it.  It never returns. */
static void idle_entry(void *arg) {
    (void)arg;

    for (;;) {
        cpu[percpu_self_index()].idle_ticks++;

        /* Sleep until something happens.  `hlt` is woken by an interrupt, and the
         * only interrupts an idle processor gets are IPIs -- the 8253's line and
         * every device line are routed to the boot processor's local APIC. */
        hlt();

        /* Woken, so look again: an IPI means something was queued for us.  If it
         * was not real work, schedule() picks this task again and returns, and we
         * come back to `hlt`. */
        schedule();
    }
}

/* The task this processor is running.  One load through %gs, which is the whole
 * reason per-processor state is reached that way. */
task_t *sched_current(void) {
    percpu_t *self = percpu_self();

    if (self == NULL)
        return NULL;
    return (task_t *)self->current;
}

/* ---------------------------------------------------------------- walking -- */

unsigned task_count(void) {
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);
    unsigned n = all_tasks.count;
    spin_unlock_irqrestore(&all_tasks.lock, flags);
    return n;
}

task_t *task_by_index(unsigned i) {
    task_t *t;

    ticket_lock(&all_tasks.lock);
    t = all_tasks.head;
    for (unsigned n = 0; t != NULL && n < i; n++)
        t = t->all_next;

    ticket_unlock(&all_tasks.lock);

    return t;
}

/* The task with this identifier.
 *
 * A linear walk rather than a table keyed by pid, for the same reason `task_by_index` is
 * one: the list is permanent and a task is never removed from it, so a table would have to
 * be a second structure kept in step with the first, and the thing that goes wrong when two
 * structures are in step by hand is that they stop being.  A process table with a few
 * hundred entries is a walk a `kill` does once. */
task_t *task_by_pid(uint64_t pid) {
    task_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);

    for (task_t *t = all_tasks.head; t != NULL; t = t->all_next) {
        if (t->id == pid) {
            found = t;
            break;
        }
    }

    spin_unlock_irqrestore(&all_tasks.lock, flags);
    return found;
}

/* The first task in creation order, and the one after it.  Together they are a walk that
 * needs no index and no count, so `ps` and the tests cannot be surprised by a task being
 * created between the two halves of an index-based walk. */
task_t *task_first(void) {
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);
    task_t *t = all_tasks.head;

    spin_unlock_irqrestore(&all_tasks.lock, flags);
    return t;
}

task_t *task_next(task_t *t) {
    if (t == NULL)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);
    task_t *next = t->all_next;

    spin_unlock_irqrestore(&all_tasks.lock, flags);
    return next;
}

/* A task's entry point, reached for the first time.  When it returns, the task is
 * finished: it is marked dead and never scheduled again. */
void task_start(void (*entry)(void *), void *arg) {
    task_t *t = sched_current();

    if (t != NULL) {
        t->started = true;
        t->last_cpu = percpu_self_index();
        t->ran_on_cpu = t->last_cpu;
    }

    entry(arg);

    task_exit();
}

task_t *task_create_idle(unsigned index) {
    task_t *t = task_create("idle", idle_entry, NULL, TASK_WEIGHT_MIN);

    if (t == NULL)
        return NULL;

    t->is_idle = true;

    /* Runnable from the start, because it is the fallback a processor runs when
     * its run queue is empty and schedule() claims it through the same
     * TASK_RUNNABLE -> TASK_RUNNING transition as anything else.
     *
     * It is never *on* a run queue: schedule() puts a task back only if it is not
     * the idle one, and rq_pick_fairest() only ever walks a queue.  That is what
     * keeps it out of the fairness comparison -- an idle task that has never run
     * has the least virtual time of anything and would otherwise win every
     * comparison and be scheduled constantly. */
    __atomic_store_n(&t->state, TASK_RUNNABLE, __ATOMIC_RELEASE);
    if (index < MAX_CPU_COUNT)
        cpu[index].idle = t;
    return t;
}

/* ---------------------------------------------------------------- wake-up -- */

int task_wakeup_on(unsigned index, task_t *t) {
    if (index >= MAX_CPU_COUNT || t == NULL)
        return -1;

    __atomic_store_n(&t->state, TASK_RUNNABLE, __ATOMIC_RELEASE);
    t->wake_at_ns = 0;
    enqueue(index, t);
    return 0;
}

int task_wakeup(task_t *t) {
    if (t == NULL)
        return -1;

    /* Hand it to the processor with the shortest queue, so waking something does
     * not pile everything onto one. */
    unsigned best = percpu_self_index();
    uint32_t best_len = MAX_CPU_COUNT;

    for (unsigned i = 0; i < percpu_count(); i++) {
        if (!percpu_is_online(i))
            continue;
        if (cpu[i].rq.length < best_len) {
            best_len = cpu[i].rq.length;
            best = i;
        }
    }

    return task_wakeup_on(best, t);
}

/* ------------------------------------------------------------- stealing --- */

/* Take one task from another processor's queue.
 *
 * Nothing is ever *pushed* between processors, so a processor that runs dry does
 * not wait behind a queue that is being filled: it goes and takes.  The victim's
 * lock is the only thing the two processors share, and it is held for as short a
 * time as the operation allows. */
static task_t *steal_from(unsigned victim) {
    sched_cpu_t *c = &cpu[victim];
    task_t *t = NULL;

    ticket_lock(&c->rq.lock);
    t = rq_pick_fairest(c);
    if (t != NULL)
        rq_remove(c, t);
    ticket_unlock(&c->rq.lock);

    if (t != NULL)
        cpu[percpu_self_index()].steals++;

    /* Never take another processor's idle task, and report nothing when the fairest
     * thing on the victim's queue is one.
     *
     * An idle task is not work.  It is what a processor runs when it has nothing else,
     * and the two are not interchangeable: a processor that steals another's idle task
     * runs it *instead of* its own, so the processor it was taken from is now the one
     * with an idle task, and the two trade places on every tick for as long as the
     * system has nothing else to run.
     *
     * At boot that trade is fatal rather than merely wasteful, because the boot task is
     * the only runnable task in the system for the first few milliseconds.  The boot
     * processor's run queue is empty the whole time, so it steals on every tick; it
     * takes an idle task, runs it, and on the next tick takes the next one -- and the
     * boot task is never picked again.  Nothing looks wrong: every processor is in its
     * idle loop and taking its timer.  The self-tests run on the boot task, so the
     * suite simply stops after the scheduler comes up, which is what it did.
     *
     * The in-schedule guard does not save this.  It defers a preemption of a processor
     * whose own queue is empty, and that is precisely the processor that is stealing an
     * idle task, so the guard never fires and there is no preemption to break the
     * cycle.
     *
     * Refusing idle tasks here costs nothing: work reaches another processor by being
     * put on that processor's queue by task_wakeup_on(), which is a direct enqueue, not
     * a steal.  Nothing is ever *published* to a run queue to be stolen from, because
     * the outgoing task is the only thing that goes on one and it is always real work.
     * If the fairest task available anywhere is an idle task, the honest answer is that
     * there is no work, and the caller falls back to this processor's own idle task. */
    if (t != NULL && t->is_idle) {
        ticket_lock(&c->rq.lock);
        rq_push(c, t);
        ticket_unlock(&c->rq.lock);
        return NULL;
    }

    return t;
}

static task_t *steal_any(void) {
    unsigned self = percpu_self_index();
    unsigned n = percpu_count();

    /* Start somewhere that is not this processor, and not always the same one: a
     * fixed victim means the first processor in line hoards every queue. */
    for (unsigned k = 1; k <= n; k++) {
        unsigned victim = (self + k) % n;

        if (victim == self || victim >= n || !percpu_is_online(victim))
            continue;

        task_t *t = steal_from(victim);
        if (t != NULL)
            return t;
    }

    return NULL;
}

/* ------------------------------------------------------------ scheduling --- */

/* Account for the time the current task has had.
 *
 * The weight is checked rather than assumed.  `elapsed / weight` is the one
 * division on the preemption path, and a division whose divisor is zero is not a
 * wrong answer, it is an exception: the processor raises #DE, the kernel's own
 * handler reports "unhandled CPU exception: vector 0", and the register dump
 * points at this function with no indication of what was wrong.
 *
 * Every path that creates a task sets a non-zero weight, so a zero one means this
 * processor's `current` does not point at a task -- the pointer itself has been
 * overwritten, or points into memory that has since been reused.  That is the bug
 * this is standing in front of; saying so by name is worth more than the divide.
 */
static void charge_current(sched_cpu_t *c) {
    task_t *t = c->current;
    uint64_t now = monotonic_ns();

    if (t == NULL || now <= c->slice_start_ns)
        return;

    if (t->weight == 0 || t->weight > TASK_WEIGHT_MAX) {
        panic("sched: processor %u has %p as its current task, and that has "
              "weight %u\n", percpu_self_index(), (void *)t, t->weight);
        return;
    }

    uint64_t elapsed = now - c->slice_start_ns;
    t->exec_ns += elapsed;
    /* Scaled by weight, so a task with four times the weight accumulates a
     * quarter of the virtual time and therefore runs four times as often. */
    t->vruntime += elapsed / t->weight;
    c->slice_start_ns = now;
}

/* Put the outgoing task back on a run queue, or do nothing with it.
 *
 * Called from task_save_and_publish() in switch.S, which has already pushed the task's
 * callee-saved registers and stored its stack pointer.  That ordering is the whole
 * reason this is a callback rather than a line of C between a save and a switch: the
 * task must not be reachable by another processor until its saved context is final, and
 * its saved context is not final until the save has happened.
 *
 * `save_rsp` is unused and takes its place only so the argument list matches the three
 * registers the assembly calls with.  Interrupts are off here -- schedule() turned them
 * off before choosing anything -- so nothing can preempt between the state store and the
 * queue insertion.
 */
/* What __enter_user saw, immediately before `iretq`.
 *
 * [0] %cs, [1] %rsp, [2..6] the five quadwords `iretq` pops: RIP, CS, RFLAGS, RSP, SS.
 *
 * It exists because the frame written by task_create_user() has been verified correct from
 * the kernel's own alias of it, and the one thing that has *not* been checked is whether
 * %rsp points at that alias when the CPU gets there.  If it does not, everything above is
 * right and the CPU still reads something else. */
uint64_t task_iret_frame[7];

void sched_publish_outgoing(uint64_t *save_rsp, task_t *prev, sched_cpu_t *c) {
    (void)save_rsp;

    /* The idle task goes back to runnable without going on a queue: it is the fallback
     * schedule() falls back to, and it is claimed through the same
     * TASK_RUNNABLE -> TASK_RUNNING transition as anything else.
     *
     * Leaving it TASK_RUNNING after switching away from it means the next time the run
     * queue is empty the claim fails, and a schedule() that fails to claim anything
     * returns *without switching*.  A task that called schedule() in order to sleep then
     * returns from its sleep still marked asleep, and the caller loops forever putting it
     * back on the blocked list. */
    if (prev->is_idle) {
        __atomic_store_n(&prev->state, TASK_RUNNABLE, __ATOMIC_RELEASE);
        return;
    }

    /* Only if it is still runnable.  The commonest reason it is not: it called
     * schedule() itself in order to sleep or block, and it is already on the blocked
     * list.  Re-queueing it would put one task on a list twice, and the second link
     * splices the whole list -- entries after the spliced point become unreachable and
     * the tasks on them never run, which is a hang rather than a wrong answer. */
    if (__atomic_load_n(&prev->state, __ATOMIC_ACQUIRE) != TASK_RUNNING)
        return;

    __atomic_store_n(&prev->state, TASK_RUNNABLE, __ATOMIC_RELEASE);
    ticket_lock(&c->rq.lock);
    rq_push(c, prev);
    ticket_unlock(&c->rq.lock);
}

void schedule(void) {
    unsigned index = percpu_self_index();

    if (index >= MAX_CPU_COUNT || !sched_up)
        return;

    sched_cpu_t *c = &cpu[index];

    /* Interrupts off across the whole selection-and-switch, and restored on the
     * way back in.  That is what makes the switch atomic against the one thing
     * that could interrupt it. */
    uint64_t flags = irq_save();

    /* Re-entering here would mean switching stacks in the middle of a switch.  The
     * flag is cleared again *before* the switch itself, which sounds contradictory
     * and is the whole trick: while it is set nothing can arrive anyway, because
     * interrupts are off, and once it is clear the task about to run must be able
     * to schedule -- a guard left set across the switch would stop the next task on
     * this processor from ever sleeping, because it would find the processor
     * "already switching" for as long as this task was away. */
    if (c->in_schedule) {
        /* The flag exists so that a preemption arriving during a switch does not
         * switch again, and `need_resched` is cleared on the way in, so returning
         * here without setting it again *loses* the request that caused this call.
         * That is not a dropped optimisation: the caller came here to sleep or to
         * yield, so nothing is switched to, it returns still asleep, and the one
         * thing that would have woken it is the very flag that was just cleared.
         *
         * So put the request back.  The outer schedule() clears the flag and switches
         * on the way out, and the next tick acts on the request that was deferred. */
        c->need_resched = 1;
        if (!c->reentry_reported) {
            c->reentry_reported = 1;
            kprintf("sched: processor %u re-entered schedule() from %p while it "
                    "was already switching (in_schedule was set); the request is "
                    "deferred to the next tick\n", index,
                    __builtin_return_address(0));
        }
        irq_restore(flags);
        return;
    }
    c->in_schedule = 1;
    c->need_resched = 0;

    charge_current(c);

    task_t *prev = c->current;
    task_t *next = NULL;

    ticket_lock(&c->rq.lock);
    next = rq_pop(c);
    ticket_unlock(&c->rq.lock);

    /* Nothing of its own: take someone else's, then fall back to idle.  Which of
     * the three the chosen task came from is recorded, because a chosen task that
     * turns out not to be a task points at one of them and nothing else does. */
    const char *source = "its own run queue";
    if (next == NULL) {
        next = steal_any();
        source = "another processor's run queue (a steal)";
    }
    if (next == NULL) {
        next = c->idle;
        source = "this processor's idle task";
    }

    if (next == NULL) {
        c->in_schedule = 0;
        irq_restore(flags);
        panic("schedule: processor %u has neither work nor an idle task\n",
              index);
        return;
    }

    /* The idle task is the fallback, so it is checked rather than assumed.  It is
     * the one candidate that is not reached by walking a list, and a captured
     * multi-processor run showed it is exactly the one that goes wrong: the task
     * `schedule()` chose was `cpu[index].idle`, and it was not a task.  A
     * non-NULL `idle` that does not look like a task means this processor's
     * scheduler state has been overwritten, and saying which field is the whole
     * difference between a bug report and a mystery. */
    if (next == c->idle && next->is_idle != true)
        panic("sched: processor %u's idle task is %p, and it is not an idle "
              "task: state %d, weight %u, id %lu\n", index, (void *)next,
              __atomic_load_n(&next->state, __ATOMIC_ACQUIRE), next->weight,
              (unsigned long)next->id);

    if (next == prev) {
        /* Getting yourself back is correct and it is what the idle task relies on:
         * there is nothing else to run, so it must not spin looking for work.  For
         * anything else it means a caller that called schedule() in order to sleep or
         * yield returned without switching, and it is still asleep -- which is a bug
         * with no symptom of its own.  So the idle task is the only one allowed to
         * land here quietly. */
        if (prev != NULL && !prev->is_idle && !c->self_returned_reported) {
            c->self_returned_reported = 1;
            kprintf("sched: processor %u chose the task it is already running "
                    "(%lu, %s, state %d); it will not be switched away from\n",
                    index, (unsigned long)prev->id, prev->name, prev->state);
        }
        c->in_schedule = 0;
        irq_restore(flags);
        return;
    }

    /* Claiming the task is an atomic transition: if another processor got there
     * first it is not ours to run, and putting it back is the correct answer
     * rather than an error.
     *
     * "Putting it back" is the important half, and it is why `next` is popped before
     * the claim rather than after: a task is off every queue while it is being
     * claimed, so a failed claim cannot leave it on two of them.  If it was popped
     * from another processor's queue by `steal_any()`, though, a failed claim does
     * leave it on *no* queue, and the processor that stole it carries on with its own
     * task instead -- which is a lost task, not a wrong one.
     *
     * So this is reported rather than swallowed.  A silent return here is
     * indistinguishable, from the outside, from a scheduler with nothing to run: no
     * switch happens, so a caller that called schedule() in order to sleep returns
     * still marked asleep.  Reported once per processor, because a burst of them is
     * one problem, not many. */
    int32_t runnable = TASK_RUNNABLE;
    if (!__atomic_compare_exchange_n(&next->state, &runnable, TASK_RUNNING,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        c->need_resched = 1;      /* try again on the way out of the tick */
        if (!c->claim_failed_reported) {
            c->claim_failed_reported = 1;
            kprintf("sched: [from %s] processor %u chose %p with state %d and id "
                    "%lu, and could not claim it -- it is not a task, or another "
                    "processor took it first.  This processor keeps its own task, so "
                    "a caller that came here to sleep returns still asleep.\n",
                    source, index, (void *)next,
                    __atomic_load_n(&next->state, __ATOMIC_ACQUIRE),
                    (unsigned long)next->id);
        }
        c->in_schedule = 0;
        irq_restore(flags);
        return;
    }

    percpu_t *self = percpu_self();
    uint32_t last = prev != NULL ? prev->last_cpu : MAX_CPU_COUNT;

    /* Everything that has to be true of the chosen task is made true here, *before*
     * the switch, because the switch below does not come back.  Each of these was
     * already done before the switch; the order between them is new, and it is new only
     * because the sequence that saves the outgoing task, publishes it, and switches can
     * no longer return into the middle of it.
     *
     * None of it needs the outgoing task's registers, which is what makes that safe: the
     * bookkeeping touches `next`, `c` and `self`, none of which live in registers that
     * get saved away. */
    c->current = next;
    self->current = next;
    next->last_cpu = index;
    next->ran_on_cpu = index;
    next->switch_count++;
    next->started = true;

    if (last != MAX_CPU_COUNT && last != index)
        c->migrations++;

    c->slice_start_ns = monotonic_ns();
    c->switches++;

    /* The address space is part of the context, so it has to be loaded before the
     * switch rather than by whoever mapped something.  vmm_address_space_switch()
     * compares first, so for the common case -- two kernel tasks, which share the
     * kernel's tables -- this is one compare and no TLB flush.
     *
     * It is *called* from inside task_save_publish_and_switch() rather than from here,
     * and that is not tidiness.  A user task's saved_rsp is a user virtual address, so
     * the pushes that save its context have to happen while its own address space is
     * still loaded; loading the incoming task's tables first means pushing onto a user
     * stack those tables do not map, which faults on the first push of every preemption
     * of a ring-3 task.  The address to load is computed here and handed over as an
     * argument. */
    page_table_t *next_tables = next->tables != NULL ? next->tables
                                                    : vmm_kernel_tables();

    /* Point this processor's RSP0 at the task that is about to run on it.
     *
     * The CPU loads RSP from TSS.RSP0 on *every* interrupt out of ring 3 -- every `int
     * $0x80`, every timer tick, every IPI -- so it has to name the task that is running
     * on this processor at this moment, and it is therefore per processor rather than per
     * task.  Set at task creation it would leave every processor pointing at the boot
     * task's stack, and a user task that migrated would take its kernel frames there.
     *
     * The boot task has no stack of its own -- it is running on the stack the kernel
     * booted on, which was allocated before there were tasks -- so `stack_top` is zero
     * and the previous value is left alone.  It is never loaded from, because the boot
     * task never leaves ring 0.
     *
     * Doing this here rather than inside the switch is safe because interrupts are off
     * across the whole selection-and-switch, so nothing can arrive between this store and
     * the switch that would use it. */
    if (next->stack_top != 0)
        tss_set_rsp0_for(index, next->stack_top);

    /* Clear the guard before switching, not after.  A task that is switched to has
     * to be able to schedule immediately -- to sleep, to yield -- and this
     * processor is now running *it*.  Nothing can arrive between here and the switch
     * because interrupts are off, and the switch itself turns none on.
     *
     * The task being switched to does not inherit the switcher's interrupt state: one
     * that has never run gets interrupts turned on where it enters, which is
     * task_enter_first() and task_start_shim() in switch.S.  That is the piece that
     * matters most here -- an idle task entered with interrupts off executes one `hlt`
     * and is never woken again, which looks exactly like a scheduler whose timer has
     * stopped. */
    c->in_schedule = 0;

    /* A task that has never run has no resume point of its own: what is on its stack is
     * the bootstrap frame task_stack_init() built, and it goes through task_enter_first(),
     * which turns interrupts on for it.
     *
     * Anything else is the save-publish-switch sequence, and there is no outgoing task
     * to publish when there was no outgoing task -- which only happens on a processor
     * that has not entered the scheduler yet, so there is nothing of its own to save. */
    if (prev == NULL) {
        /* The address space is loaded here because the sequence that normally does it is
         * not on this path.  That path is for a processor that has not entered the
         * scheduler yet, so it has no outgoing task to save -- and skipping the load
         * there means the task starts on whatever tables the processor already had.
         *
         * It used to be reachable at run time as well, because task_exit() cleared
         * `current` on its way out; task_exit() no longer does that.  Both halves are
         * needed: this one because the path exists at all, and the other because a task
         * that starts on the wrong tables faults on its first memory access and says
         * nothing about why. */
        vmm_address_space_switch(next_tables);
        task_enter_first(next->saved_rsp);
    } else {
        /* Whether the outgoing context is a ring-3 frame is recorded by the interrupt
         * entry, not derived here: by the time schedule() runs the CPU has already
         * switched to CPL 0, so the current CS cannot say.  Consuming it here is what
         * stops it leaking into a later save that really was from the kernel. */
        int32_t from_user = self->from_user;

        /* The ring-3 context itself, if there is one: the whole block the interrupt
         * entry was handed, recorded there and consumed here together with the flag.
         * The handler that pushed it has built frames of its own below it by now, so
         * schedule() cannot find it any other way -- and saving the block whole is what
         * carries the nine general registers that are not callee-saved across the
         * switch.  See task_save_publish_and_switch(). */
        const void *out_ctx = self->user_frame;

        self->from_user = 0;
        self->user_frame = NULL;

        task_save_publish_and_switch(&prev->saved_rsp, prev, c, next->saved_rsp,
                                     next_tables, &prev->saved_in_user,
                                     &next->saved_in_user, from_user, out_ctx);
    }

    /* Only reached when this task is switched back into.
     *
     * So the ring-3 marker has to be cleared *here*, on the way in, rather than on the
     * way out: the task may come back on a processor that never saw it block, and that
     * processor's marker is whatever its last ring-3 interrupt left there.  A task running
     * in ring 0 saved against a stale marker is saved as though a ring-3 frame were on its
     * stack, and comes back through `iretq` off the middle of its own C frames.
     *
     * After the switch, not before: the save above reads the marker to decide how to save
     * *this* task, and clearing it there would save every ring-3 task as though it were in
     * the kernel. */
    self_from_user_clear();

    irq_restore(flags);
}

void sched_yield(void) {
    unsigned index = percpu_self_index();

    if (index < MAX_CPU_COUNT)
        cpu[index].need_resched = 1;
}

/* -------------------------------------------------------------- the tick -- */

/* Wake anything whose sleep has expired, and hand it to a processor with room. */
static void wake_sleepers(void) {
    uint64_t now = monotonic_ns();
    task_t *wake[16];
    unsigned n = 0;

    ticket_lock(&blocked.lock);
    for (task_t *t = list_head_checked(blocked.head, &blocked.head,
                                       "blocked list");
         t != NULL && n < 16; ) {
        /* The link to follow has to be taken *before* the removal, because
         * removing clears exactly that link -- reading it afterwards ends the walk
         * after one task, so a burst of sleepers would wake one per tick and the
         * rest would sit there until the machine happened to be quiet. */
        task_t *next = list_next_checked(t, &blocked.head, "blocked list");

        if (t->wake_at_ns != 0 && t->wake_at_ns <= now) {
            blocked_remove(t);
            wake[n++] = t;
        }
        t = next;
    }
    ticket_unlock(&blocked.lock);

    for (unsigned i = 0; i < n; i++) {
        int32_t sleeping = TASK_SLEEPING;
        if (__atomic_compare_exchange_n(&wake[i]->state, &sleeping,
                                        TASK_RUNNABLE, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            enqueue(percpu_self_index(), wake[i]);
        }
    }
}

void sched_tick(void) {
    unsigned index = percpu_self_index();

    /* Before the run queues exist there is nothing to preempt onto, and a tick
     * still has to arrive and be acknowledged for the interrupt path to be worth
     * anything. */
    if (index >= MAX_CPU_COUNT || !sched_up)
        return;

    sched_cpu_t *c = &cpu[index];
    c->ticks++;

    sched_check_stack();
    wake_sleepers();

    /* Preempt when the slice is used up or when something asked for it.  The
     * idle task is the exception: it has no slice to lose, and switching it out
     * and straight back in on every tick would be pure overhead. */
    task_t *t = c->current;
    if (t != NULL && t->is_idle) {
        /* An idle processor still has to notice work arriving on another
         * processor's queue, so it looks, but it does not switch on its own. */
        if (cpu[index].rq.length > 0)
            schedule();
        return;
    }

    uint64_t now = monotonic_ns();
    if (now - c->slice_start_ns >= SCHED_SLICE_NS || c->need_resched)
        schedule();
}

/* ------------------------------------------------------------- re-parenting --- */

/* Make `orphan` a child of `new_parent`, on both sides of the link.
 *
 * Two lists are involved and both have to move, which is the whole reason this is a
 * function: `parent->child` is the list `wait` walks, and `orphan->sibling` is how an
 * orphan is on it.  Fixing one without the other leaves a task that `wait` can find but
 * nothing points at, or a task whose `parent` says one thing and whose `sibling` says
 * another -- and the second of those is how a `wait(-1)` ends up returning the same child
 * twice.
 *
 * Detaching from the old parent is done by identity, so a caller does not have to know
 * whether `orphan` is the old parent's first child, its third, or the only one. */
static void reparent(task_t *orphan, task_t *new_parent) {
    task_t *old = orphan->parent;

    if (old != NULL) {
        task_t **linkp = &old->child;

        while (*linkp != NULL && *linkp != orphan)
            linkp = &(*linkp)->sibling;

        if (*linkp == orphan)
            *linkp = orphan->sibling;
    }

    orphan->sibling  = new_parent->child;
    new_parent->child = orphan;
    orphan->parent   = new_parent;

    /* The group and session follow the parent, because a process's controlling terminal is
     * its session's, and a job-control shell that does not know which session a grandchild
     * is in cannot send it a ^C. */
    orphan->pgrp    = new_parent->pgrp;
    orphan->session = new_parent->session;
}

/* Hand every task whose parent has died to PID 1.
 *
 * Called when a task exits, before the exiting task itself leaves the tree.  The rule is
 * the one every Unix uses: a child whose parent is gone belongs to init, so that something
 * is left to wait on it and report its status.
 *
 * Without this the orphans are not merely unreported -- they are *unreachable*.  `wait`
 * walks `parent->child`, so a task whose `parent` pointer names a task that has already gone
 * is on nobody's list of children: init's `wait(-1)` does not see it, `ps` sees a process
 * whose parent does not exist, and the task's stack and address space are never freed.  A
 * shell running a background job and then exiting leaves one of those behind every time,
 * which is a leak only a reboot clears.
 *
 * The walk is over `all_tasks` rather than over the tree, because a tree walk from the
 * exiting task would miss orphans whose parent died in an earlier exit and were never
 * adopted -- and it has to be a walk rather than a single fixup, because the exiting task
 * may itself be an ancestor of the ones being adopted.
 *
 * Called with `all_tasks.lock` held and interrupts off.  Every link written here is also
 * read by `sys_wait`, by procfs and by `fork`, none of which take a lock, so the only thing
 * that makes a re-parent atomic is not being interrupted in the middle of it. */
static void adopt_orphans(task_t *dying) {
    /* PID 1.  Found by walking the list rather than kept as a pointer, because a cached one
     * would have to be invalidated if init ever exited -- and init does not exit, which is
     * precisely why it is worth finding rather than caching. */
    task_t *init = NULL;

    for (task_t *w = all_tasks.head; w != NULL; w = w->all_next)
        if (w->id == 1) {
            init = w;
            break;
        }

    if (init == NULL)
        return;                 /* no init yet: nothing to adopt to, and nothing is lost */

    /* The exiting task first, and then everything else.
     *
     * Doing the exiting task before its descendants matters: a child of it would otherwise
     * be adopted by the loop, and then have its `parent` overwritten by this, and the two
     * would have to be undone against each other. */
    if (dying != NULL && dying != init && dying->parent != NULL &&
        dying->parent != init && dying->parent->state == TASK_DEAD)
        reparent(dying, init);

    for (task_t *w = all_tasks.head; w != NULL; w = w->all_next) {
        if (w == init)
            continue;

        /* `state == TASK_DEAD` is the test for "gone".  A parent that is merely blocked,
         * stopped or waiting is not an orphan, and adopting its children would take away the
         * parent's ability to wait on them. */
        if (w->parent == NULL || w->parent->state != TASK_DEAD)
            continue;

        if (w->parent == init)
            continue;           /* already adopted; a second pass would splice it twice */

        reparent(w, init);
    }
}

void task_exit(void) {
    unsigned index = percpu_self_index();
    task_t *t = sched_current();

    if (t != NULL) {
        __atomic_store_n(&t->state, TASK_DEAD, __ATOMIC_RELEASE);
        /* And /proc loses it.  A /proc that keeps an entry for every process the machine
         * has ever run is a filesystem that fills up, and a shell that lists it gets a
         * list of things that died last week -- so the entry goes when the process does.
         * After the `exited` flag, because a parent that reads status and finds the
         * directory already gone should see the status it was promised, not a missing
         * file. */
        procfs_forget_task(t);

        /* Whatever this task owned becomes init's, and so does this task if its own parent
         * has already gone.  After the state store, because "is my parent dead" is answered
         * by that field and it has to be answered before the question is asked. */
        uint64_t flags = spin_lock_irqsave(&all_tasks.lock);

        adopt_orphans(t);
        spin_unlock_irqrestore(&all_tasks.lock, flags);
    }

    /* Nothing will ever run this processor again unless it has an idle task, and
     * an idle task that exits would leave the processor with nothing. */
    if (index >= MAX_CPU_COUNT || t == NULL || t->is_idle) {
        for (;;)
            hlt();
    }

    /* Leave through schedule() as an ordinary switch away from a task that is no
     * longer runnable, which is what this is.
     *
     * It used to clear `current` first, so that schedule() would see no outgoing task
     * and take the first-entry path instead.  That is wrong for a reason that only shows
     * up with two tasks on one processor: the first-entry path enters a task that has
     * never run, and it deliberately does *not* load the chosen task's address space --
     * that is done by the save-publish-switch sequence, which the first-entry path does
     * not go through.  So the next task started up and ran on the dead one's page tables,
     * mapped its page into its own, walked its own tables to find it, and then took a
     * page fault writing to it.  The walk and the fault disagreed about which address
     * space was loaded, which is a confusing enough report to be worth writing down.
     *
     * As an ordinary switch-away, the context is saved -- which costs nothing for a task
     * that will never be resumed -- and sched_publish_outgoing() declines to queue it
     * because its state is no longer TASK_RUNNING.  That is what that state check is
     * for, and this is the case it was written for. */
    schedule();

    for (;;)
        hlt();
}

/* ------------------------------------------------------------- sleeping --- */

void task_sleep_ms(uint64_t ms) {
    task_t *t = sched_current();

    if (t == NULL)
        return;

    /* Interrupts off across the whole of "decide to block" *and* the switch.
     *
     * Leaving them on leaves a window that nothing can recover from.  Between
     * putting the task on the sleep list and switching away, a tick can preempt
     * this task -- and since it is already TASK_SLEEPING it is deliberately not
     * put back on the run queue, so it is on the sleep list and running nowhere.
     * Another processor's tick then wakes it, queues it and runs it; when it gets
     * back here it is running somewhere it never switched to, and its call to
     * schedule() resumes inside a scheduler that has since moved on.  The visible
     * symptom is a task linked into two lists at once, which corrupts the run
     * queue it was queued on. */
    uint64_t flags = irq_save();

    uint64_t lflags = spin_lock_irqsave(&blocked.lock);

    __atomic_store_n(&t->state, TASK_SLEEPING, __ATOMIC_RELEASE);
    t->wake_at_ns = monotonic_ns() + ms * 1000000ull;
    blocked_add(t);

    spin_unlock_irqrestore(&blocked.lock, lflags);
    self_from_user_clear();
    schedule();

    irq_restore(flags);
}

/* Put the running task on the blocked list without yet switching away.
 *
 * Split out of task_wait() so a caller that has to decide *and* register atomically can:
 * see wait_queue_block_locked(), where the window between the two is a lost wake. */
void task_block_register(void) {
    task_t *t = sched_current();

    if (t == NULL)
        return;

    uint64_t lflags = spin_lock_irqsave(&blocked.lock);

    __atomic_store_n(&t->state, TASK_WAITING, __ATOMIC_RELEASE);
    t->wake_at_ns = 0;          /* nothing but a named wake will end this */
    blocked_add(t);

    spin_unlock_irqrestore(&blocked.lock, lflags);
}

/* Wait for a named wake, with no condition of its own to re-check. */
void task_wait(void) {
    if (sched_current() == NULL)
        return;

    /* Interrupts off for the same reason as in task_sleep_ms(): the window
     * between blocking and switching away must not be interruptible. */
    uint64_t flags = irq_save();

    task_block_register();
    self_from_user_clear();
    schedule();

    irq_restore(flags);
}

/* ---------------------------------------------------------- wait queues --- */

void wait_queue_init(wait_queue_t *q) {
    ticket_lock_init(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    for (unsigned i = 0; i < WAIT_QUEUE_ENTRIES; i++) {
        q->slots[i].tag = 0;
        q->slots[i].task = NULL;
    }
}

uint32_t wait_queue_count(const wait_queue_t *q) { return q->count; }

/* Block until this queue is woken with `tag`.
 *
 * The check for a wake and the act of sleeping cannot be one operation, so a
 * wake can arrive in between.  The loop is what makes that safe: the slot is
 * marked under the queue's lock, and the task only blocks if the mark is not
 * already there.  Losing that race means returning immediately, not a missed
 * wake-up. */
void wait_queue_sleep(wait_queue_t *q, unsigned tag) {
    task_t *self = sched_current();
    uint32_t slot;

    if (self == NULL)
        return;

    ticket_lock(&q->lock);
    if (q->count == WAIT_QUEUE_ENTRIES) {
        ticket_unlock(&q->lock);
        /* Full is a bug in the caller, and a dropped waiter is a task that never
         * wakes, so it is said rather than worked around. */
        panic("wait_queue: %u waiters, more than the %u slots\n", q->count,
              (unsigned)WAIT_QUEUE_ENTRIES);
        return;
    }
    slot = q->tail;
    q->tail = (q->tail + 1) % WAIT_QUEUE_ENTRIES;
    q->slots[slot].tag = tag;
    q->slots[slot].task = self;
    q->count++;
    ticket_unlock(&q->lock);

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&q->lock);
        bool woken = q->slots[slot].tag == 0;
        spin_unlock_irqrestore(&q->lock, flags);

        if (woken) {
            uint64_t f2 = spin_lock_irqsave(&q->lock);
            q->slots[slot].task = NULL;
            q->slots[slot].tag = 0;
            q->count--;
            spin_unlock_irqrestore(&q->lock, f2);
            return;
        }

        task_wait();
    }
}

/* Enqueue without blocking.  The caller must hold q->lock, which is what makes the pair
 * with wait_queue_block_locked() safe: see the header. */
uint32_t wait_queue_enqueue_locked(wait_queue_t *q, unsigned tag) {
    uint32_t slot = q->tail;

    if (q->count == WAIT_QUEUE_ENTRIES) {
        /* Full is a bug in the caller, and a dropped waiter is a task that never wakes. */
        panic("wait_queue: %u waiters, more than the %u slots\n", q->count,
              (unsigned)WAIT_QUEUE_ENTRIES);
        return q->head;
    }

    q->tail = (q->tail + 1) % WAIT_QUEUE_ENTRIES;
    q->slots[slot].tag = tag;
    q->slots[slot].task = sched_current();
    q->count++;

    return slot;
}

/* Drop the lock and block until the slot is woken, then reclaim it.
 *
 * The caller must hold q->lock and must not touch what it was holding afterwards: the lock
 * is dropped on the way in, and everything this waits on is guarded by it. */
void wait_queue_block_locked(wait_queue_t *q, uint32_t slot) {
    /* Interrupts off across the whole of it, for the reason task_wait() gives: the window
     * between deciding to block and switching away must not be interruptible. */
    uint64_t flags = irq_save();
    task_t *self = sched_current();

    for (;;) {
        /* Entered with `q->lock` held; each pass leaves it held again.  The test and the
         * registration below happen with that lock still held, which is what closes the
         * lost wake: a waker needs the same lock to touch the slot, so it either arrives
         * first -- and the tag is already zero, so nothing is registered -- or after, when
         * the task is already TASK_WAITING and `unblock()` queues it. */
        bool woken = q->slots[slot].tag == 0;

        if (woken) {
            q->slots[slot].task = NULL;
            q->slots[slot].tag = 0;
            q->count--;

            /* `count` is the queue's occupancy and the rest of this file believes it: the
             * enqueue hands out a slot on the strength of it.  An undercount hands out a
             * slot a live waiter still holds, and that surfaces as an indirect call
             * through an overwritten pointer rather than as anything to do with a queue. */
            if (q->count > WAIT_QUEUE_ENTRIES)
                panic("wait_queue: count %u after reclaiming a slot, more than the %u\n",
                      (unsigned)q->count, (unsigned)WAIT_QUEUE_ENTRIES);

            ticket_unlock(&q->lock);
            irq_restore(flags);
            return;
        }

        task_block_register();

        ticket_unlock(&q->lock);

        /* Claimed between the unlock and here?  `unblock()` clears the tag under `q->lock`
         * before its CAS, so a task no longer in TASK_WAITING has been woken.  Switching
         * away now would leave it queued *and* blocked -- one task with two owners, which
         * is what `list_link()` panics on -- so the wait ends and the caller re-tests its
         * own condition against whatever woke it. */
        if (self == NULL ||
            __atomic_load_n(&self->state, __ATOMIC_ACQUIRE) != TASK_WAITING) {
            uint64_t f2 = spin_lock_irqsave(&q->lock);

            q->slots[slot].task = NULL;
            q->slots[slot].tag = 0;
            q->count--;
            spin_unlock_irqrestore(&q->lock, f2);
            irq_restore(flags);
            return;
        }

        self_from_user_clear();
        schedule();

        /* Back from `schedule()`, so the task was queued and has run.  Take the lock for
         * the next test -- a wake only means "something changed", and a spurious one must
         * send this back round rather than reclaim a slot nothing woke. */
        spin_lock_irqsave(&q->lock);
    }
}

/* Wake the first waiter whose tag matches; tag 0 means "any". */
void wait_queue_wake(wait_queue_t *q, unsigned tag) {
    task_t *woken[WAIT_QUEUE_ENTRIES];
    unsigned n = 0;

    ticket_lock(&q->lock);
    /* The whole array, not a window from `head` -- see the header. */
    for (unsigned i = 0; i < WAIT_QUEUE_ENTRIES && n < 4; i++) {
        if (q->slots[i].task == NULL)
            continue;
        if (tag != 0 && q->slots[i].tag != tag)
            continue;

        /* Zero is the woken marker; the waiter clears it before returning. */
        if (q->slots[i].tag == 0)
            continue;
        q->slots[i].tag = 0;
        woken[n++] = q->slots[i].task;
        if (tag != 0)
            break;
    }
    ticket_unlock(&q->lock);

    for (unsigned i = 0; i < n; i++)
        unblock(woken[i]);
}

void wait_queue_wake_all(wait_queue_t *q) {
    task_t *woken[WAIT_QUEUE_ENTRIES];
    unsigned n = 0;

    ticket_lock(&q->lock);
    /* The whole array, not a window from `head` -- see the header for why `head` cannot
     * say where the live waiters are. */
    for (unsigned i = 0; i < WAIT_QUEUE_ENTRIES && n < 8; i++) {
        if (q->slots[i].task != NULL && q->slots[i].tag != 0) {
            q->slots[i].tag = 0;
            woken[n++] = q->slots[i].task;
        }
    }
    ticket_unlock(&q->lock);
    for (unsigned i = 0; i < n; i++)
        unblock(woken[i]);
}

/* ------------------------------------------------------------ diagnostics -- */

void sched_get_stats(unsigned index, sched_stats_t *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    if (index >= MAX_CPU_COUNT)
        return;

    sched_cpu_t *c = &cpu[index];

    out->ticks = c->ticks;
    out->switches = c->switches;
    out->steals = c->steals;
    out->migrations = c->migrations;
    out->idle_ticks = c->idle_ticks;
    out->queued = c->rq.length;

    uint64_t flags = spin_lock_irqsave(&blocked.lock);
    out->sleeping = blocked.length;
    spin_unlock_irqrestore(&blocked.lock, flags);

    out->online = percpu_online_count();
}

/* --------------------------------------------------------------- console -- */

const char *task_state_name(int state) {
    switch (state) {
    case TASK_UNUSED:   return "unused";
    case TASK_NEW:      return "new";
    case TASK_RUNNABLE: return "runnable";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "asleep";
    case TASK_WAITING:  return "blocked";
    case TASK_BLOCKED:  return "blocked";
    case TASK_DEAD:     return "dead";
    default:            return "?";
    }
}

/* The per-processor view, for `sched`.
 *
 * Printed per processor rather than summed: the interesting question about a
 * scheduler is not "how much did it do" but "is any processor not doing any",
 * and a sum hides that completely.  An idle processor with no switches is not a
 * fault -- it has nothing to switch to -- which is why its idle count is the
 * number worth reading. */
void sched_report(void) {
    unsigned n = percpu_online_count();

    kprintf("scheduler: %u processor(s) online, %u task(s), slice %lu us\n",
            n, task_count(), (unsigned long)(SCHED_SLICE_NS / 1000));

    for (unsigned i = 0; i < n; i++) {
        sched_stats_t st;

        sched_get_stats(i, &st);
        kprintf("  cpu %u: %lu ticks, %u switches, %u steals, %u migrations, "
                "%u queued, %u idle wakeups%s\n",
                i, (unsigned long)st.ticks, st.switches, st.steals,
                st.migrations, st.queued, st.idle_ticks,
                st.switches == 0 ? "  (idle)" : "");
    }

    uint64_t flags = spin_lock_irqsave(&blocked.lock);
    unsigned sleeping = blocked.length;
    spin_unlock_irqrestore(&blocked.lock, flags);
    kprintf("  %u task(s) asleep or blocked\n", sleeping);
}

/* A stack overflow that is allowed to happen quietly takes the machine down a
 * long way from its cause, so the tick checks. */
void sched_check_stack(void) {
    task_t *t = sched_current();
    uint64_t rsp;

    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));

    if (t == NULL || t->stack_guard == NULL)
        return;               /* the boot processor's own stack has no guard */

    /* A user task that has entered ring 3 runs its *kernel* code on its own user stack,
     * because `iretq` puts %rsp there and the kernel half of the address space is mapped
     * all the way down.  So %rsp is a user address, which is far below any kernel heap,
     * and comparing it with the kernel stack's bottom reports an overflow that has not
     * happened -- on the first ring-3 entry, every time.
     *
     * It reported one.  The panic it produced is what `phase6-user` had been dying of:
     * a stack-overflow report naming an address in the middle of the user stack and a
     * "bottom" in the kernel heap, and then a panic handler running on the user stack,
     * which is why the follow-on reports were all nonsense -- a return address of
     * TASK_STACK_GUARD, a register dump with a kernel-text `%rbp`, and a serial log that
     * stopped in the middle of a line.  Every one of those was a symptom of panicking on
     * a stack the panic path cannot use.
     *
     * There is nothing to check here: the user stack has no guard word, and its bounds are
     * the loader's business, not the tick's.  A kernel task's %rsp is never a user
     * address, so this does not weaken the check for anybody else. */
    if (t->is_user && vmm_is_user_addr(rsp))
        return;

    if (rsp < t->stack_low)
        panic("sched: task %lu (%s) has overflowed its stack: rsp %#lx is below "
              "the bottom at %#lx\n", (unsigned long)t->id, t->name,
              (unsigned long)rsp, (unsigned long)t->stack_low);

    if (*t->stack_guard != TASK_STACK_GUARD)
        panic("sched: task %lu (%s) has overwritten its stack guard at %#lx "
              "(found %#lx)\n", (unsigned long)t->id, t->name,
              (unsigned long)(uintptr_t)t->stack_guard,
              (unsigned long)*t->stack_guard);
}

/* ------------------------------------------------------------------ init -- */

void sched_init(void) {
    percpu_t *self = percpu_self();

    ticket_lock_init(&blocked.lock);
    ticket_lock_init(&all_tasks.lock);

    /* Every processor the firmware described gets a queue and an idle task now,
     * even the ones that have not come online yet: an application processor's
     * first act on arrival is to be put on a queue that already exists, and a
     * queue created at that moment would be a queue nobody else knows about.
     *
     * Queues exist for every slot, because a queue costs one lock, but an idle
     * task costs a 16 KiB stack and there are exactly as many of those as there
     * are processors that can ever run. */
    for (unsigned i = 0; i < MAX_CPU_COUNT; i++) {
        ticket_lock_init(&cpu[i].rq.lock);
        cpu[i].slice_start_ns = monotonic_ns();
    }

    /* The boot processor's own context becomes a task, so it can be preempted and
     * resumed exactly where it was -- which is the only way the console and
     * everything hanging off it keep working across a time slice. */
    task_t *boot = task_alloc();
    if (boot == NULL)
        panic("sched_init: no memory for the boot task\n");

    strlcpy(boot->name, "boot", sizeof(boot->name));
    __atomic_store_n(&boot->state, TASK_RUNNING, __ATOMIC_RELEASE);
    boot->weight = TASK_WEIGHT_MAX;    /* the console must not be starved */
    boot->stack = NULL;                 /* it keeps the boot stack it is on */
    boot->stack_top = 0;
    boot->saved_rsp = 0;                /* filled in by the first switch out */
    boot->last_cpu = percpu_self_index();
    boot->ran_on_cpu = percpu_self_index();
    boot->started = true;

    task_register(boot);

    for (unsigned i = 0; i < percpu_count(); i++)
        task_create_idle(i);

    unsigned index = percpu_self_index();
    cpu[index].current = boot;
    self->current = boot;

    /* Last, so no tick can find half a scheduler. */
    __atomic_store_n(&sched_up, 1, __ATOMIC_RELEASE);

    kprintf("sched: run queues for %u processor(s), boot task %lu, idle tasks "
            "%lu, slice %lu us\n", percpu_count(), (unsigned long)boot->id,
            (unsigned long)cpu[0].idle->id, (unsigned long)(SCHED_SLICE_NS / 1000));
}

/* An application processor's first act.  It has no task of its own, so it starts
 * as the idle task of its own queue -- which is what it was doing before -- and
 * is switched off it the moment there is real work.  Never returns. */
void sched_task_enter(unsigned index) {
    percpu_t *self = percpu_self();

    if (index >= MAX_CPU_COUNT || cpu[index].idle == NULL) {
        kprintf("[warn] sched: processor %u has no idle task; spinning\n",
                index);
        for (;;)
            hlt();
    }

    sched_cpu_t *c = &cpu[index];
    task_t *idle = c->idle;

    __atomic_store_n(&idle->state, TASK_RUNNING, __ATOMIC_RELEASE);
    idle->last_cpu = index;
    idle->ran_on_cpu = index;
    idle->started = true;
    c->current = idle;
    self->current = idle;
    c->slice_start_ns = monotonic_ns();

    kprintf("sched: processor %u entering the scheduler as idle task %lu\n",
            index, (unsigned long)idle->id);

    /* Jump straight onto the idle task's prepared stack: it has never run, so
     * there is nothing to save here. */
    task_enter_first(idle->saved_rsp);

    for (;;)
        hlt();
}
/* --- fork --------------------------------------------------------------- */

/* The size of a `regs_t` in quadwords, and where `%rax` sits inside it.
 *
 * Both belong next to the code that copies a frame rather than in cpu.h, because they are
 * only meaningful to a caller that is copying one -- which is this, and the ISR tail. */
#define REGS_WORDS       22
#define REGS_RAX_INDEX   14        /* r15, r14, ... rbx, rax: fifteen, so rax is the 15th */

task_t *task_create_forked(const char *name, const regs_t *regs, uint32_t weight) {
    task_t *t = NULL;
    uint64_t flags = spin_lock_irqsave(&all_tasks.lock);

    t = (task_t *)kmalloc(sizeof(task_t));
    spin_unlock_irqrestore(&all_tasks.lock, flags);

    if (t == NULL)
        return NULL;

    /* Zeroed, which is what every list and pointer in task_t wants to start as. */
    memset(t, 0, sizeof(*t));

    /* The current task's descriptors, copied.  A fork inherits open files; that is what
     * makes a pipe usable across one, since both processes then hold a descriptor for the
     * same pipe.  Copied rather than shared, so a child that closes its end does not close
     * the parent's -- the pipe counts its open ends, and `pipe_close_end()` is what makes
     * that count drop to nothing.
     *
     * Read from the running task rather than passed in, because there is one parent by
     * definition -- it is whichever task called `fork` -- and passing it would be a
     * parameter that can only ever have one value. */
    fd_entry_t *parent_fds = fd_table();

    if (parent_fds != NULL)
        fd_table_copy(t->fds, parent_fds);

    /* The signal dispositions and the blocked set are inherited; nothing *pending* is.
     *
     * That is POSIX's rule and it is the right one: a child does not inherit a signal that
     * its parent had not yet been told about, because it was not its signal yet.  It also
     * removes a real hazard -- a SIGCHLD the parent has been sent but not delivered is
     * still pending in the parent, and copying it would give the child a `wait` that
     * returns for a child that is not its own. */
    task_t *parent = sched_current();

    if (parent != NULL) {
        memcpy(t->sig_slots, parent->sig_slots, sizeof(t->sig_slots));
        t->sig_blocked = parent->sig_blocked;

        /* The child's directory, its ids and its mask are the parent's.
         *
         * `task_create()` gives all of these to a task it makes, and this is not that
         * function -- a forked task is built here, from `kmalloc` and a `memset`, which is
         * why it needed saying at all.  Without it the child starts in a current directory
         * of *nothing*: `getcwd()` answers ENOENT, every relative path fails with ENOENT,
         * and `cd /tmp` followed by `pwd` reports "no such file or directory" from inside a
         * directory that exists.  Which is what a shell does when `init` forks it, so this
         * is the difference between a shell and a shell that cannot find its own files. */
        strlcpy(t->cwd, parent->cwd, sizeof(t->cwd));
        t->cwd_ino = parent->cwd_ino;
        t->cwd_mount = parent->cwd_mount;
        t->uid = parent->uid;
        t->gid = parent->gid;
        t->umask = parent->umask;
    }

    /* Registered rather than given an identifier here: `task_register()` is what puts a
     * task on the all-tasks list and assigns the id, and a task that has an id but is not
     * on that list is invisible to everything that walks it -- `ps`, and the test that
     * checks the list's own invariants. */
    task_register(t);

    strncpy(t->name, name, sizeof(t->name) - 1);
    t->is_user = true;
    t->weight = weight;
    t->state = TASK_NEW;

    /* Its own group and session.  Its `/proc` entry was made by `task_register()`, which
     * is where its identifier came from and where every other list that names it is built;
     * doing it here as well would have meant a second rule about when a process exists,
     * and the two would not have agreed. */
    t->pgrp = (uint32_t)t->id;
    t->session = (uint32_t)t->id;

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);

    if (stack == NULL)
        return NULL;

    /* Aligned so the frame lands where `iretq` wants it: sixteen-byte aligned, with the
     * guard at the very bottom so the first byte written past the stack reports it. */
    uint64_t *frame = (uint64_t *)(((uint64_t)stack + TASK_STACK_SIZE) & ~15ull) - REGS_WORDS;

    memcpy(frame, regs, REGS_WORDS * sizeof(uint64_t));

    /* The child sees zero from `fork`, and the parent sees the child's identifier.  The
     * return value is the only register that differs between them, which is why this is a
     * copy of the parent's frame and not something assembled afresh. */
    frame[REGS_RAX_INDEX] = 0;

    *(volatile uint64_t *)stack = TASK_STACK_GUARD;
    t->stack_guard = (volatile uint64_t *)stack;
    t->stack_low = (uint64_t)stack;
    t->stack = stack;
    t->stack_top = (uint64_t)stack + TASK_STACK_SIZE;

    /* The scheduler's ring-3 restore path: this block *is* the interrupt frame, so
     * `saved_rsp` names it and `saved_in_user` says so.  Nothing to build at the top of a
     * user stack here, because there is no entry stack -- the child already has one, and
     * it is the parent's, untouched. */
    t->saved_rsp = (uint64_t)frame;
    t->saved_in_user = 1;

    return t;
}

int task_attach_cow_twin(task_t *child) {
    task_t *parent = sched_current();

    if (child == NULL || parent == NULL || parent->tables == NULL)
        return -1;

    cow_list_t *cow = (cow_list_t *)kmalloc(sizeof(cow_list_t));

    if (cow == NULL)
        return -1;

    page_table_t *tables = (page_table_t *)kmalloc(sizeof(*tables));

    if (tables == NULL || vmm_fork_aspace(tables, parent->tables, cow) != 0) {
        if (cow->full) {
            kprintf("[warn] task: fork would need more than %d copy-on-write pages; "
                    "refusing rather than sharing a page it should have copied\\n",
                    COW_MAX_PAGES);
        }
        return -1;
    }

    child->tables = tables;
    child->cow = cow;
    child->parent = parent;

    /* The parent needs a list as well, and this is the half that is easy to miss.
     *
     * `vmm_fork_aspace()` takes write permission away from *both* sides -- that is what
     * makes the pair copy-on-write rather than a child protected and a parent writing
     * straight through to the shared frame.  So the parent now faults on the first write to
     * anything it had writable, exactly as the child does, and with no list to consult the
     * kernel has nothing to resolve it with and reports a page fault that is really just
     * `fork` having happened.
     *
     * The entries are the same original leaf values on both sides and `vmm_cow_fault()`
     * only reads the list, so the parent can be given a copy.  It cannot be given *the*
     * list: the two tasks are freed independently.
     *
     * And if the parent is itself a forked child it already has a list, for the pages it
     * gave up to *its* parent.  Those entries are still needed, so this adds to it rather
     * than replacing it -- which is the general rule: a task's list is every page it has
     * given up, and a fork gives up more. */
    if (parent->cow == NULL) {
        cow_list_t *own = (cow_list_t *)kmalloc(sizeof(cow_list_t));

        if (own == NULL)
            return -1;

        memcpy(own, cow, sizeof(*own));
        parent->cow = own;
    } else if (parent->cow->count + cow->count <= COW_MAX_PAGES) {
        memcpy(&parent->cow->page[parent->cow->count], &cow->page[0],
               cow->count * sizeof(cow_page_t));
        parent->cow->count += cow->count;
    } else {
        kprintf("[warn] task: the parent already owes copy-on-write copies of %u "
                "pages and cannot owe %u more; refusing the fork rather than "
                "sharing pages it should have copied\n",
                parent->cow->count, cow->count);
        return -1;
    }

    /* Onto the parent's child list.  `sibling` is this child's link and `child` the
     * parent's head, so a fork from a task that already has children is a prepend and
     * costs nothing -- and `wait` scans the list, so the order children were made in is
     * not the order they exit in, which is exactly what a parent asking "wait for any"
     * expects. */
    child->sibling = parent->child;
    parent->child = child;

    return 0;
}

void task_exit_with(int status) {
    task_t *t = sched_current();

    if (t != NULL) {
        /* Tell the parent, here, rather than leaving it to whichever way this task
         * happened to die.
         *
         * A parent blocked in `wait` is off the run queue with nothing to bring it back,
         * so a child that exits without waking it stops the whole process tree at the
         * first child -- and reports nothing, because nothing has gone wrong: the parent
         * is asleep waiting for an event that already happened.  `sys_exit` does this too,
         * which is harmless because waking a task that is not blocked does nothing; it is
         * here because a task can also die without ever calling `exit` at all -- killed by
         * a signal whose default action is to terminate, which is what a `sh` running
         * `sleep 100 &` and then being told to stop does to that process. */
        task_t *parent = t->parent;

        t->exit_code = status;

        /* Publish the status before the flag that says it is there.  A parent on another
         * processor reads `exited` and then `exit_code`, and the other order lets it see
         * "finished" with the status the task has not written yet -- which is a value from
         * whatever ran on that task's slot last. */
        __atomic_store_n(&t->exited, 1, __ATOMIC_RELEASE);

        if (parent != NULL)
            task_unblock(parent);
    }

    task_exit();
}
