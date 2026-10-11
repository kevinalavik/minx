/* minx - kernel tasks and the scheduler.
 *
 * Phase 5.  A task is a kernel thread: a stack, some registers, and a place in a
 * run queue.  Nothing here is a user-space process yet -- that is phase 6 and
 * arrives as an address space attached to the same structure, so this one does
 * not have to be thrown away.
 *
 * The shape of it:
 *
 *   - Each processor has its own run queue and its own idle task.  A run queue is
 *     protected by one lock, and it is the *only* lock in the scheduler: no lock
 *     is ever held across a context switch, which is what keeps the whole thing
 *     free of the deadlock a "big kernel lock" version would have.
 *   - A processor that finds its own queue empty tries to take work from another
 *     one.  Nothing is pushed between processors, so there is never a queue to
 *     drain backwards and no processor waits for another to finish.
 *   - Fairness is by virtual runtime, which is the CFS idea without the rbtree:
 *     each task accumulates time scaled by its weight, and the runnable task with
 *     the least is chosen.  It is O(tasks), which is fine at this scale and is
 *     named in DECISIONS.md as the thing to replace when there are thousands.
 */
#ifndef MINX_TASK_H
#define MINX_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "pagetables.h"
#include "percpu.h"
#include "spinlock.h"
#include "cpu.h"
#include "signal.h"
#include "vmm.h"

/* Task states.  A task is on exactly one list -- a run queue, a sleep list, a
 * wait queue, or nowhere -- and its state is what says which. */
enum {
    TASK_UNUSED     = 0,
    TASK_NEW        = 1,   /* built, never entered                        */
    TASK_RUNNABLE    = 2,   /* on a run queue, waiting for a processor      */
    TASK_RUNNING    = 3,   /* the task on some processor right now         */
    TASK_SLEEPING   = 4,   /* on the sleep list until its timer expires    */
    TASK_WAITING    = 5,   /* on a wait queue until something wakes it      */
    TASK_BLOCKED    = 6,   /* waiting on a lock or a queue, not timed       */
    TASK_DEAD       = 7,
};

/* Weights, not priorities.  A task with four times the weight gets four times
 * the processor, which is the same statement as a priority four steps better and
 * does not need the scheduler to know anything about who set it. */
#define TASK_WEIGHT_MIN     1
#define TASK_WEIGHT_DEFAULT 4
#define TASK_WEIGHT_MAX     64

/* What a descriptor names, and the table a task keeps them in.
 *
 * These live here rather than in fd.h because task_t embeds the table and fd.h needs
 * `wait_queue_t` from this file, so the header that defines them cannot include fd.h
 * without going in a circle.  fd.h includes this one and is what programs of the kernel
 * use. */
typedef enum {
    FD_NONE = 0,
    FD_CONSOLE,      /* the framebuffer and its serial mirror, as one thing */
    FD_PIPE,         /* pipe_t, one particular end of it */
} fd_kind_t;

/* Which end of a pipe a descriptor is, so a number cannot be used for the other end by
 * accident. */
typedef enum {
    PIPE_READ = 0,
    PIPE_WRITE = 1,
} pipe_end_t;

typedef struct {
    fd_kind_t kind;
    uint8_t   pipe_end;      /* which end, when kind is FD_PIPE */
    void     *obj;           /* the pipe, when kind is FD_PIPE */
    bool      closable;      /* false for 0, 1 and 2: they are not ours to close */
} fd_entry_t;

/* How many descriptors a task can hold.  Fixed rather than allocated because the table is
 * read on every read and every write, and a kernel with no pipes yet had exactly three
 * numbers to work with; a pipe is two more and they have to come from somewhere. */
#define FD_TABLE_ENTRIES 16

typedef struct task {
    uint64_t     id;
    char         name[16];
    void        *stack;          /* the kernel stack, NULL if it uses its own */
    uint64_t     stack_top;
    uint64_t     saved_rsp;      /* the context: callee-saved regs and a ret */

    /* This task's own address space, or NULL to mean the kernel's.
     *
     * NULL rather than a pointer to the kernel's tables for a kernel task, so that
     * "is this a user task" is one comparison and does not have to compare against
     * something that moves.  vmm_tables() is the only thing that should read it, and
     * it turns NULL into the kernel's tables.
     *
     * Owned by the task: created with it, and released when the task is destroyed.
     * Nothing in the scheduler shares one, because sharing one would mean a mapping
     * one process makes appears in all of them. */
    page_table_t *tables;

    /* Set for a task that entered ring 3, so the exception and page-fault paths can
     * tell a user address from a kernel one without trusting the faulting `cs`. */
    bool         is_user;

    /* The parent, for `wait`, and the children waiting on it.
     *
     * NULL for the boot task and for a kernel task, which have no parent to wait on.
     * `wait` walks `children` and takes the first that has exited, which is why it is a
     * list rather than a single pointer: several children can be outstanding and the
     * order they exit in is not the order they were made in. */
    struct task *parent;
    struct task *sibling;          /* the next child of the same parent */
    struct task *child;            /* the first child of this task */

    /* Set once the task has called `exit`, with the status it passed, so that "has it
     * finished" and "how did it finish" are one question with one answer rather than two
     * that can disagree.  A parent blocked in `wait` reads both; the self tests read
     * `exit_code` on its own. */
    volatile int32_t exited;

    /* The pages this address space gave up write permission on for its twin, or NULL.
     *
     * Owned by the task and freed with it.  A page fault consults it through the running
     * task, because a fault happens on whatever task is running and the tables it is
     * faulting on are that task's. */
    cow_list_t  *cow;

    /* The program's own idea of how it went, from its `exit`, and the top of its heap
     * from `brk`.  Read by the self tests, and by /proc later; a task's exit status is
     * the one piece of state a program cannot set for itself. */
    int          exit_code;
    uint64_t     brk;

    volatile int state;
    uint32_t     weight;
    uint64_t     vruntime;       /* scaled time spent running */
    uint64_t     exec_ns;        /* real time spent running */

    /* Timed sleep.  Monotonic, so a change to the wall clock cannot make a
     * sleeper wait for hours. */
    uint64_t     wake_at_ns;

    /* Links for the transient lists: a run queue, or the blocked list.  A task is
     * on at most one of them at a time, and `list_tag` says which, as the address
     * of that list's head -- the scheduler's tables are per processor, so the
     * address identifies the list uniquely.
     *
     * `all_next` is separate and is not for a transient list.  The list of every
     * task is permanent: a task is in it from creation until the machine stops,
     * while the run queue links are rewritten every time it is queued.  Sharing
     * one pair of links between the two is not a shortcut that happens to work --
     * queueing a task overwrites the links the task list is threaded through, so
     * the next task registered walks into the run queue instead, and the run
     * queue's length stops matching what it holds. */
    struct task *next;
    struct task *prev;
    struct task *all_next;
    uint64_t      list_tag;

    /* Diagnostics, and the only way the self tests can tell that a task really
     * ran rather than that it was merely enqueued. */
    uint64_t     switch_count;   /* how many times it was switched *into* */
    uint64_t     ran_on_cpu;     /* last processor it actually ran on */
    uint32_t     last_cpu;

    void       (*entry)(void *);
    void        *arg;
    bool         is_idle;
    bool         started;

    /* The low address of this task's stack, and a magic word at that address.
     *
     * Both exist because a kernel stack that overflows does not fault.  It grows
     * down into whatever the allocator gave it below, and the first things it
     * destroys are saved return addresses -- so the symptom is not a stack fault
     * but a jump into the middle of the heap, or a page fault thousands of
     * instructions later on some unrelated data structure.  `sched_tick()` checks
     * %rsp against the low address every tick, and the magic word catches an
     * overflow that crosses it in one go. */
    uint64_t     stack_low;
    volatile uint64_t *stack_guard;

    /* A kernel alias of this task's ring-3 return frame, for a user task only; NULL
     * for a kernel task.
     *
     * The frame sits at the top of the task's own user stack, which exists only in that
     * task's address space -- so the kernel cannot reach it through its own page tables,
     * and `phys_to_virt()` of the *user* address does not work either: that is just
     * arithmetic on a user address, and it yields a kernel address with no page behind
     * it.  What does work is the direct map of the frame's own physical page, which is
     * what this holds.  It exists because the loader has to patch RIP and RSP into the
     * frame after the ELF is read, and that happens well after the task is created. */
    uint64_t    *user_frame;

    /* The physical address of the *bottom* page of this task's user stack, and how many
     * pages there are.  Both exist for the same reason as `user_frame`: the stack is
     * mapped only in this task's address space, so the kernel reaches it through the
     * direct map of its own frames, and the ELF loader has to zero it and build the
     * argv/envp/auxv block in it. */
    uint64_t     user_stack_phys;
    uint32_t     user_stack_pages;

    /* This task's open descriptors, and nothing else's.
     *
     * A fork copies the entries and shares the objects they name, which is the whole
     * reason a pipe survives one: both processes hold a descriptor for the same pipe, so a
     * write on one side is a read on the other.  Copying rather than sharing the table is
     * also why a child that closes its copy does not close the parent's -- the kernel
     * counts the ends that are open on each object instead of trusting a number to mean
     * "still open". */
    fd_entry_t   fds[FD_TABLE_ENTRIES];

    /* Signals: what this task has been sent, what it is ignoring, and what it has
     * installed.  See signal.h for what happens next.
     *
     * `sig_trampoline` is a *program* address, not a kernel one: the handler is entered
     * exactly as `call` would enter it, so its return address has to be somewhere ring 3
     * can execute, and the only such address a program knows is one it supplied.  A
     * kernel trampoline would have to live at a ring-3 address, and the kernel is mapped
     * high where ring 3 cannot reach.
     *
     * `sig_pending` is set by another processor with an atomic, hence the volatile pair:
     * a task can be sent a signal while it is running, and the value it reads at the end
     * of an interrupt has to be the one that was there. */
    uint64_t     sig_handler[SIG_MAX];
    uint64_t     sig_trampoline[SIG_MAX];
    volatile uint64_t sig_pending;
    uint64_t     sig_blocked;

    /* Whether the context in `saved_rsp` is a ring-3 frame rather than a set of
     * callee-saved registers and a return address.
     *
     * A task interrupted while running in ring 3 gets a *full* return frame pushed by the
     * CPU -- RIP, CS, RFLAGS, SS, RSP -- above the point the handler runs at, because the
     * CPU switched to the kernel stack first.  A kernel task gets no such frame: its
     * callee-saved registers and its own return address are the whole context, and it is
     * resumed with a `ret`.
     *
     * Resuming a user task's context with `ret` pops the ring-3 RIP as though it were a
     * return address and jumps to it in ring 0, with the kernel's stack, the kernel's CS
     * and everything the ring-3 frame said about SS and RSP discarded.  The symptom is not
     * a fault at that address; it is arbitrary behaviour for as long as the next fault
     * takes to arrive, which is why this needs a flag rather than a check.
     *
     * Written by task_save_publish_and_switch() in switch.S, read by nothing but the
     * restore there -- the C side cannot know the processor's current privilege level. */
    int32_t      saved_in_user;
} task_t;

/* The word written at the bottom of every task stack. */
#define TASK_STACK_GUARD 0x5dead5deadbeef11ull

/* How many tasks this kernel will have at once.
 *
 * There is no table to overflow -- tasks are heap-allocated -- so this is a limit on
 * memory rather than on indices, and it exists because `fork` in a loop would otherwise
 * take the heap to zero and fault somewhere with nothing to connect it to the loop.  The
 * cost is a forked task's kernel stack, so the number is generous: 16 KiB each, and phase
 * 11's stress test forks in the dozens, not the hundreds. */
#define TASK_LIMIT 512

/* Report a stack overflow loudly rather than letting it corrupt something else.
 * Called from the tick, so it costs two loads and a compare. */
void sched_check_stack(void);

/* --- life cycle --------------------------------------------------------- */

/* The task the calling processor is running right now.  Never NULL after
 * sched_init(): the boot processor's own context is wrapped as a task so that it
 * can be preempted like anything else and resumed exactly where it was. */
task_t *sched_current(void);

/* Wrap the calling processor's current context as a task, create the run queues
 * and the idle task for every processor, and start scheduling on processor 0. */
void sched_init(void);

/* Called by an application processor as it comes online: create its idle task,
 * make it current, and enter the scheduler.  Never returns. */
void sched_task_enter(unsigned index);

/* A new kernel task.  `entry` is called with `arg` the first time it is
 * scheduled; when it returns, the task is marked dead and never runs again.
 * Returns NULL if there is no room. */
task_t *task_create(const char *name, void (*entry)(void *), void *arg,
                    uint32_t weight);
task_t *task_create_idle(unsigned index);

/* A new task that starts in ring 3.
 *
 * Its own address space is created for it, and its stack is laid out with a return
 * frame that puts it in user code at `entry`: the user code selector in CS, the user
 * data selector in SS, a user stack pointer at the top of its own stack, and IF set
 * so its first interrupt is its own choice.
 *
 * `stack_pages` is rounded up to a whole number of pages, which are allocated, zeroed
 * and mapped user-writable at the top of the user address space.  They are *not*
 * mapped for the kernel: a kernel thread must not be able to write a user stack
 * through the direct map, which is what CR4.SMAP is for.
 *
 * Returns NULL if the address space, the stack or the task cannot be allocated. */
task_t *task_create_user(const char *name, uint64_t entry, uint32_t stack_pages,
                         uint32_t weight);

/* Put a task on some processor's run queue and make sure a processor is awake to
 * run it.  Wakes an idle processor if the one woken has nothing to do. */
int  task_wakeup(task_t *t);
int  task_wakeup_on(unsigned index, task_t *t);

/* Wake a task that is *blocked* -- one in task_wait(), or sleeping.
 *
 * Not the same as task_wakeup(), and using that one here is a crash rather than a
 * mistake that shows up: a blocked task is linked into the blocked list, and a run queue
 * refuses a task that is already linked into a list (list_link() checks, because one task
 * on two lists splices them).  So waking a blocked parent with task_wakeup() makes the next
 * queue insertion panic on the parent's name -- which is what `fork` followed by `wait`
 * did, at the point the child exited and told its parent so.
 *
 * A no-op if the task is not blocked, so a wake that races another one, or a timed sleep
 * that has already fired, is harmless rather than a double insertion. */
void task_unblock(task_t *t);

/* Mark a task dead.  Its stack is not freed: there is nothing to free it into
 * safely, and a leaked 16 KiB is better than a use-after-free. */
void task_exit(void);

/* Exit with a status, which is what `exit` wants: task_exit() exists to end a kernel
 * thread that has nobody waiting for it, and has no status to report because no one is
 * looking. */
void task_exit_with(int status);

/* A forked child: a kernel stack holding a frame that returns 0 into the same place its
 * parent will return from, over an address space that is its parent's twin.
 *
 * Not built by task_create_user() because that one lays out an *entry* stack for a program
 * starting for the first time, and this is the opposite: the child continues partway
 * through an instruction stream its parent is already executing, with every register
 * except the return value the same. */
task_t *task_create_forked(const char *name, const regs_t *regs, uint32_t weight);

/* Attach a copy-on-write twin of `parent`'s address space to a freshly forked task.  Kept
 * apart from task_create_forked() so that a failure to build the twin leaves nothing to
 * unwind except the task itself. */
int  task_attach_cow_twin(task_t *child);

/* The two frames a user task starts with, and their sizes.
 *
 * A user task's stack holds, at the very top, the scheduler's saved context (six
 * callee-saved registers and a return address) directly below which is the ring-3 return
 * frame that `iretq` pops: RFLAGS, CS, RIP, SS, RSP.  task_create_user() writes both from
 * these sizes, and task_enter_user() in switch.S adds USER_CTX_BYTES to the stack pointer
 * to get from one to the other -- so they cannot drift apart silently. */
#define USER_FRAME_BYTES  (5 * sizeof(uint64_t))
#define USER_CTX_BYTES    (7 * sizeof(uint64_t))

/* --- scheduling --------------------------------------------------------- */

void schedule(void);

/* Ask for a reschedule at the next tick.  Safe from any context: it sets a
 * flag, not a stack. */
void sched_yield(void);

/* The timer tick.  Counts it, wakes anything whose sleep has expired, and
 * preempts if the current task has used its slice.  Called from interrupt
 * context. */
void sched_tick(void);

/* --- sleeping ----------------------------------------------------------- */

/* Sleep for `ms` milliseconds.  Returns early if woken. */
void task_sleep_ms(uint64_t ms);

/* Block until something calls task_wakeup() on this task. */
void task_wait(void);

/* Put the calling task on the blocked list without switching away, so a caller that has
 * to test a condition and register atomically can.  `task_wait()` is this plus
 * `schedule()`. */
void task_block_register(void);

/* --- wait queues -------------------------------------------------------- */

/* A wait queue of tagged waiters.  Tags are the caller's own identifiers -- a
 * address, an index, a pointer -- so a wake can name one waiter rather than
 * everybody.  The slot count is fixed so that blocking never has to allocate,
 * which it could not safely do in a path that might already hold a lock. */
#define WAIT_QUEUE_ENTRIES 64

/* One waiter's place in a queue.
 *
 * `tag` is the waiter's own identifier -- an index, a pointer, an address -- so a
 * wake can name one waiter instead of everybody.  It doubles as the wake marker: a
 * slot is waiting while its tag is set and no longer, and the waiter clears it
 * before it returns.  That is what makes a wake that arrives between the check
 * and the sleep safe: the waiter finds the mark already gone and returns. */
typedef struct wait_slot {
    unsigned tag;
    task_t  *task;
} wait_slot_t;

typedef struct wait_queue {
    ticket_lock_t lock;
    uint32_t      head;
    uint32_t      tail;
    uint32_t      count;
    wait_slot_t   slots[WAIT_QUEUE_ENTRIES];
} wait_queue_t;

/* Both wake functions scan the whole slot array rather than a window starting at
 * `head`, and `head` is not a cursor.
 *
 * It looks like the front of the queue and is not: wait_queue_init() sets it to zero and
 * nothing ever advances it, because a slot is not retired in the order it was queued.  It
 * is retired by whichever task gets woken reclaiming it, and that can be any of them.
 * The live waiters are therefore scattered anywhere in the array, and the gap a retired
 * slot leaves is counted as though it were still occupied.
 *
 * Scanning `[head, head + count)` therefore reads the wrong slots as soon as one waiter in
 * the middle goes away, and the symptom is a wake that finds nobody while somebody is
 * demonstrably waiting: a pipe whose reader has given up its slot and whose writer is
 * blocked on a full pipe both sit there for ever, neither faults and neither panics, and
 * it looks like a scheduler that has stopped making progress.
 *
 * Sixty-four slots is sixty-four words, on a path taken once per state change rather than
 * once per byte, so the whole-array scan is free -- and it cannot be wrong the way a
 * cursor has to be maintained under concurrent retirement. */
void wait_queue_init(wait_queue_t *q);
void wait_queue_sleep(wait_queue_t *q, unsigned tag);

/* Enqueue without blocking, then block.  Both require the caller to already hold
 * `q->lock`.
 *
 * wait_queue_sleep() cannot be used by anything that decides to block based on state held
 * under a *different* lock, because it enqueues and blocks as one step: a waker that
 * changes that state in between -- after the caller has decided to wait and before the
 * slot exists -- wakes nobody, and the caller sleeps for ever with the condition already
 * satisfied.
 *
 * Splitting the two closes that window, because the caller holds the lock that guards the
 * condition for the whole of it.  A pipe is the case in point: its buffer is guarded by
 * the queue's own lock, so a reader that enqueues under that lock cannot be woken before
 * it is enqueued, and a waker needs the same lock to change anything.
 *
 * `wait_queue_block_locked()` drops the lock on the way in and does not take it again, so
 * the caller must not touch anything it was holding. */
uint32_t wait_queue_enqueue_locked(wait_queue_t *q, unsigned tag);
void     wait_queue_block_locked(wait_queue_t *q, uint32_t slot);
void wait_queue_wake(wait_queue_t *q, unsigned tag);
void wait_queue_wake_all(wait_queue_t *q);
uint32_t wait_queue_count(const wait_queue_t *q);

/* --- looking at tasks ---------------------------------------------------- */

/* How many tasks exist, and the `i`th of them.  The order is creation order, so
 * index 0 is the boot task and the idle tasks follow.  Reads are locked, so a
 * task created while a walk is in progress cannot be missed halfway -- the count
 * and the walk are two separate calls and the second may see more. */
unsigned task_count(void);
task_t  *task_by_index(unsigned i);

/* --- diagnostics -------------------------------------------------------- */

typedef struct sched_stats {
    uint64_t ticks;              /* preemption ticks taken               */
    uint32_t switches;           /* context switches this processor made */
    uint32_t steals;             /* tasks taken from another run queue    */
    uint32_t migrations;         /* times this processor changed tasks    */
    uint32_t idle_ticks;         /* ticks spent with nothing to run       */
    uint32_t queued;             /* tasks on this processor's run queue   */
    uint32_t sleeping;           /* tasks on the sleep list               */
    uint32_t online;             /* processors the scheduler knows about  */
} sched_stats_t;

void sched_get_stats(unsigned index, sched_stats_t *out);

/* What `ps` prints in the state column. */
const char *task_state_name(int state);

/* The per-processor scheduler view, for the console's `sched` command. */
void sched_report(void);

#endif /* MINX_TASK_H */