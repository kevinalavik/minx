# LOCKING

What the kernel synchronises with, in what order, and why. Read this before taking
a lock: two of the primitives here will let a processor that already holds one
wait for it forever, and neither of them says so at the point where it happens.

## The primitives

`kernel/include/spinlock.h` is the interface; `kernel/lib/spinlock.c` is what they
compile to. There are five kinds and the difference between them is not what is
protected but what the holder is allowed to do while it has it.

| Kind | Waits by | Interrupts | Use it for |
|------|----------|------------|------------|
| `ticket_lock` | spinning, in ticket order | on | any short critical section |
| `mcs_lock` | spinning, in arrival order | on | the same, when the waiters contend enough that one shared word is the bottleneck |
| `spin_lock_irqsave` | spinning | **off** | anything an interrupt handler can also take |
| `mutex` | spinning | on | longer sections; becomes a real sleep in phase 5 |
| `semaphore` | spinning | on | producer/consumer, and counts |

There is no wait queue yet. A wait queue needs somewhere to put a waiter that is
not burning a processor, and that is the scheduler (phase 5). The mutex and the
semaphore are honest about the same limit: they give up mutual exclusion but not
the processor, which is correct and is the right choice for a short section and
the wrong one for anything that can block.

## Order

Only one rule, and it is the usual one:

> **Take locks in increasing address order. Never take two at once.**

There is no nesting anywhere in the kernel today, so nothing depends on this yet —
which is exactly when to write it down, because the first nested pair is the one
that turns a deadlock into a hang somebody has to debug with a machine that will
not stop.

Where more than one lock is needed for a single operation, the operation should be
one lock instead. `percpu_queue_work()` takes a per-CPU slot with a
compare-and-swap and no lock at all, because each slot has exactly one legitimate
concurrent writer per direction.

## Per-CPU state is not a lock

`%gs` resolves to the calling processor's own slot in one instruction, and the
slot is that processor's to write. There is nothing to lock, and nothing should be
locked:

- Reading your own slot needs no lock.
- Reading *another* processor's slot needs no lock only if the field is written
  solely by its owner and readers tolerate a stale value. `percpu_ipis()`,
  `percpu_interrupts()` and `percpu_idle_ticks()` are all of that kind, which is
  why `cpus` can print them for every processor from any processor.
- Writing another processor's slot *is* shared, and goes through the queue
  (`percpu_queue_work`) or an atomic. The `pending` word is the claim: a compare
  and-swap, not a lock, because one slot with one writer is not a lock's job.

## Why the interrupt-save variant is not optional

If a processor holding a lock takes an interrupt and the handler takes the same
lock, it spins forever waiting for itself. x86-64 has no instruction that means
"take this lock and raise the priority above the level that would take it again",
so the only defence is to have interrupts off while the lock is held.

That is the whole of `spin_lock_irqsave()`, and it hands the saved flags back to
the caller rather than restoring "interrupts on" unconditionally — restoring the
*saved* state is what makes it safe to take a lock that was already held with
interrupts off, which is what a handler does.

Anything a device can interrupt while it is held needs it. Anything only other
processors contend for does not, and there it costs a serialising instruction per
critical section for nothing.

## Two things that will hang the machine

Both are in the code, both are commented where they are, and both are the kind of
bug that shows up as "the machine is busy and nothing happens":

- **Never take a non-recursive lock twice.** `mutex_lock()` checks and panics,
  because a lock taken twice by one processor never becomes free again: the second
  acquire waits for a release that cannot come. The check is a comparison against
  the owning processor, which is why `mutex_t` carries an owner at all.
- **Always end an interrupt.** Every local APIC handler writes the end-of-interrupt
  register, including the spurious one. The interrupt-acknowledge sequence sets an
  in-service bit and nothing else clears it; leave it set and the processor
  refuses every further interrupt of the same priority, which for the local APIC
  means all of them — its timer, every IPI, every device line. It does not crash
  and it does not report anything, it just stops being interrupted.

## Contention, measured

`make test` runs the locks against every online processor at once
(`phase4-atomics`, `phase4-locks`, `phase4-lockfair`, `phase4-mcs`,
`phase4-mutex`, `phase4-semaphore`, `phase4-irqsave`). Each one is a shared
counter that must end up with exactly what the sum of every processor's work says,
because a lock that loses an update shows up there and nowhere else.

Verified on 1, 2, 3, 4 and 8 processors. 32000 contended critical sections across
four processors complete in single-digit milliseconds.
## The scheduler

One run queue per processor, one ticket lock each, and one idle task per processor.
The rule that makes the whole thing work:

> **The queue lock is taken, the queue is examined, and the lock is dropped before
> anything switches. The context switch takes no locks at all.**

So there is no lock held across a switch, none held across anything that can block,
and no ordering to get wrong between two run queues — there is no ordering, because
there is only ever one lock in hand. `steal_from()` borrows the *victim's* queue lock
for the shortest time the operation allows and takes nothing else while holding it.

What the rule costs, paid knowingly: a task chosen for a processor can be taken by
another processor between the moment the queue lock is dropped and the moment it is
switched to. `task_t.state` is the arbiter — only one processor ever wins the
`TASK_RUNNABLE → TASK_RUNNING` transition, and the loser gives the task back rather
than reporting an error.

Three rules the switch itself imposes, each of which was a hang before it was a rule:

- **Nothing is ever pushed between processors.** A processor that runs dry steals; it
  never waits behind a queue that is being filled.
- **A task switched to must not inherit the switcher's interrupt state.** With
  interrupts off across the switch, a task with a saved context brings its own flags
  back, and one with no saved context gets them turned on where it enters
  (`task_enter_first` and `task_start_shim` in `switch.S`). A task entered with IF
  clear that then calls `hlt` never wakes, and its processor stops taking interrupts
  without reporting anything.
- **Interrupts stay off across "decide to block" *and* the switch** (`task_sleep_ms`,
  `task_wait`). With them on, a tick can preempt a task between the moment it is on
  the sleep list and the moment it switches away — and since it is deliberately not
  put back on the run queue, it is then on the sleep list and running nowhere.

One task is on at most one transient list at a time, and `list_tag` says which; the
permanent task list has its own link (`all_next`), because sharing one pair of links
between the two silently splices one into the other the first time a task is queued
after being registered. `rq_push()` and `rq_remove()` check both invariants and say
which task and which list rather than failing later somewhere unrelated.

`sched_get_stats()` reads the per-processor counters without their locks. That is a
benign race for anything printed; a *self* check must not do it. `percpu_self_snapshot()`
exists because reading `%gs` twice to check that GS is self-consistent looks like a
consistency check and is not one — a task can be migrated between the two reads, and
the report then says GS points at the wrong state when it pointed at the right one and
the task simply moved.
