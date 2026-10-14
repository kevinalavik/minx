# DECISIONS
Format: date, decision, alternatives considered, reason. Deprecated items are kept, not deleted.

## 2026-10-03 — Limine 12.9.1 as the bootloader
Decision: boot with Limine in its native `limine` protocol, loading the kernel
as an ELF.
Alternatives: GRUB multiboot2, writing our own bootloader, the `linux` protocol.
Reason: Limine already hands over a memory map, a framebuffer, the RSDP, SMP
per-CPU entry points and arbitrary modules (the initrd) in long mode, which is
exactly what SPEC.md asks for and is a large amount of firmware work to
reproduce. The native protocol takes an ELF, which matters because the kernel
is linked at absolute higher-half addresses. Limine's BIOS payload is
MIT-licensed and the release ships the `limine` host tool's *source*, so
`limine bios-install` is built with the host compiler instead of being
downloaded.

## 2026-10-03 — Own framebuffer terminal instead of Flanterm
Decision: render the console ourselves (`kernel/drivers/fb.c` +
`kernel/drivers/console.c`) on top of a converted PSF2 bitmap font.
Alternatives: Flanterm, drawing to Limine's own terminal.
Reason: SPEC.md allows "Flanterm or own renderer + bitmap font". The console is
the single most important debugging surface for the whole project, and owning it
means a kernel panic, an early fault and the TTY layer all report through exactly
the same code path with no third-party state to reason about. It also keeps the
build free of a PNG decoder dependency. Flanterm remains the obvious candidate
if later phases need terminal features (alternate screen, scrollback) that turn
out to be more trouble to add than to adopt.

## 2026-10-03 — Font is never scaled
Decision: the console always uses the font's native 8x16 cells, whatever the
framebuffer resolution.
Alternatives: pick the largest integer scale that still leaves a usable grid
(an earlier version of this code), or scale to fill the screen.
Reason: scaling up wastes most of a high-resolution framebuffer on very large
cells. A 1280x800 framebuffer gives a 160x50 terminal, which is more useful
than 40x12 with chunky text.

## 2026-10-03 — `__kernel_end` is the exclusive end of the mapped image
Decision: the linker script defines `__kernel_end` as the address just past the
last byte of `.bss`, i.e. exactly `p_vaddr + p_memsz` of the final PT_LOAD.
Alternatives: rounding it up to the next page, which is the more usual habit.
Reason: the byte at the page-rounded address is outside the segment the
bootloader maps, so *reading* `__kernel_end` faults. The rounded-up variant
cost us a silent triple fault during bring-up. The symbol is therefore only ever
used for address comparisons, never dereferenced.

## 2026-10-03 — Own free-list allocator, heap carved out of an explicit region list
Decision: first-fit free list with full coalescing, over regions registered with
`kmalloc_attach_region()`.
Alternatives: dlmalloc, a buddy allocator, a slab cache over a fixed arena.
Reason: phase 1 needs an allocator before the PMM exists, so the API is "hand me
regions" rather than "go find memory". The implementation is small enough to
audit, and it is exercised by a host-compiled fuzz test (20 000 randomised
alloc/free rounds) which is how the region-head leak and a split that dropped
`b->next` were found. dlmalloc can be slotted in behind the same interface
later if fragmentation proves to matter.

## 2026-10-03 — Test results are printed by the guest, parsed by a host script
Decision: the kernel prints `PASS`/`FAIL` lines and a tally; `tools/run-tests.sh`
greps them, cross-checks the tally against the line count, and dumps the
framebuffer through the QEMU monitor.
Alternatives: a test harness inside the kernel, or parsing screenshots.
Reason: the guest cannot meaningfully report on itself after a panic, and the
serial log is the one channel that survives a crashed console. The framebuffer
dump is checked separately because SPEC.md requires the graphical console, and a
passing serial log alone would not prove it.

## 2026-10-03 — Bring the descriptor tables up before anything else
Decision: `kmain()` installs the GDT (with TSS) and the IDT before touching any
other subsystem, and the exception path prints through a serial-only fallback
until the console exists.
Alternatives: deferring descriptor setup until after the console is up.
Reason: without an IDT any fault is a triple fault and the machine dies silently.
The serial fallback in `putc_raw()` is what lets a fault during bring-up report
itself at all.

## 2026-10-03 — `-Wall -Wextra` clean from the start, not tidied up later
Decision: the build already uses `-Wall -Wextra` for all first-party code.
Alternatives: adding them once the code is "finished".
Reason: warnings about unused variables and sign mismatches caught real bugs
during bring-up (a `uint16_t` selector passed to `ltr`, a duplicated boot
record), and they are cheaper to fix the moment they appear.

## 2026-10-03 — GCC fallback if clang is broken on the host
Decision: the Makefile prefers clang and falls back to gcc when
`clang --version` does not work.
Alternatives: requiring clang outright.
Reason: AGENTS.md asks for clang, and it is the default here, but a host with a
mismatched `llvm-libs` package should still be able to build the kernel.

## 2026-10-03 — The HHDM offset comes from Limine, not a constant
Decision: request `limine_hhdm_request` and use the offset the bootloader
returns, in `kernel/mm/hhdm.c`.
Alternatives: hard-coding `0xffff800000000000`.
Reason: the offset is a property of the bootloader's build, not of the
architecture. A kernel that assumes it computes correct addresses for a machine
that chose differently, and the failure appears far from the cause: page-table
frames that resolve to the wrong physical addresses, a direct map that maps the
wrong thing. Taking it from the bootloader turns a silent assumption into a
value the log can print.

## 2026-10-03 — The kernel's physical address is discovered, not computed
Decision: find where the bootloader actually put the image by walking its own
page tables (`vmm_walk_translate`), instead of assuming
`image_virt - 0xffffffff80000000`.
Alternatives: asking the bootloader, or hard-coding the load address.
Reason: Limine places the image at an address derived from the firmware's
preferences, and it is not the one the link script suggests — on this machine
the image is at physical `0x1f92a000`, not at zero. The arithmetic is wrong in a
way that produces a plausible-looking physical address, so it is used to *check*
the answer rather than to produce one.

## 2026-10-03 — No PML4 self-mapping; prove the tables are inside the direct map
Decision: no self-reference at `PML4[PML4_INDEX(HHDM_OFFSET)]`. Instead
`tables_under_direct_map()` checks that every page-table frame the allocator has
handed out is below the top of the direct map.
Alternatives: the conventional self-map, which is what `vmalloc()`-style
allocators use.
Reason: the kernel image and the direct map share PML4 slot 511, so a
self-reference written there overwrites the direct map's own entry. Every kernel
address then walks to "not present" and the machine triple-faults on the very
first `CR3` load. The direct map already covers every frame the allocator can
return, so the check is a proof rather than a duplication: if a frame ever falls
outside it the kernel says so instead of faulting later.

## 2026-10-03 — The bootloader's reclaimable memory is held across the CR3 switch
Decision: `pmm_init()` holds every reclaimable range before the CR3 switch and
`pmm_release_boot_tables()` frees them afterwards, both in `kernel/mm/pmm.c`.
Alternatives: freeing them as soon as they are seen.
Reason: Limine's page tables are still the active CR3 when the allocator first
learns about those ranges. Releasing them early hands out frames that the
page-table walk is reading, and the result is a fault inside the allocator with
no useful context.

## 2026-10-03 — The ISA IRQ to IOAPIC pin map is probed, not assumed
Decision: default to the identity map (pin *n* aliases ISA IRQ *n*), then use the
8253 to find its own pin at boot by masking every other pin in turn and seeing
which one answers. A line whose pin is taken that way reports itself
unroutable so `irq_enable()` fails rather than pretending.
Alternatives: identity only; or a machine-type quirk table.
Reason: the identity map is what the architecture suggests and what real
hardware does, but QEMU's IOAPIC model routes the 8253's IRQ0 to pin 2, and an
IOAPIC delivers a vector rather than the pin it came from — so a wrong pin is
indistinguishable from a line that never fires. A quirk table fixes QEMU and
breaks the next machine; a probe is right on both, costs about a tenth of a
second, and turns a class of silent bug into a boot-log line.

## 2026-10-03 — LINT0 in fixed delivery, legacy lines edge-triggered
Decision: LINT0 is unmasked in fixed (not ExtINT) delivery, and the sixteen
legacy ISA lines are routed edge-triggered.
Alternatives: leaving LINT0 in ExtINT as the firmware set it, and routing the
legacy lines level-triggered.
Reason: ExtINT makes the APIC take the vector from its own LVT entry and ignore
the one in the message, so every device arrives on the same vector and the
dispatcher cannot tell a keystroke from a clock tick. And an ISA interrupt is a
pulse: as a level-triggered entry the chip latches it in its remote-IRR bit and
re-asserts it for as long as the line stays high, so any device that leaves its
line asserted — a PS/2 controller with a byte in its output buffer, an 8253 that
holds its output pin high — produces an endless stream of interrupts nobody
asked for. Level-triggered entries are for PCI devices with an interrupt-status
register.

## 2026-10-03 — The TSC is measured against the 8253, not taken from CPUID
Decision: `timer_init()` runs the 8253's channel-2 gate window (65536 ticks =
65536/1193182 s of exactly known time), counts TSC ticks across it, and installs
the result through `delay_set_tsc_hz()`. The CPUID leaves are tried first and
kept in the boot log.
Alternatives: believing CPUID; assuming 1 GHz when it says nothing; calibrating
against the LAPIC timer.
Reason: all three CPUID leaves that can report a TSC frequency are optional and
reported zero on the machine this was developed on. The fallback was wrong in
the direction that makes delays *short*, which is the direction nothing notices:
a timeout fires early and looks like a fast machine. The LAPIC timer cannot be
the reference either, because it is itself measured and the two would only be
compared against each other. The 8253 is the one clock here that depends on no
frequency guess.

## 2026-10-03 — The bitmap physical allocator, not a buddy allocator
Decision: one bit per 4 KiB frame over the firmware's usable ranges
(`kernel/mm/pmm.c`), with contiguous runs available because page tables and large
mappings need them.
Alternatives: a buddy allocator, dlmalloc's mmap-like path, one of the allocators
named in AGENTS.md.
Reason: it is small enough to read in one sitting, every operation is O(1), and
it cannot fail to coalesce. Fragmentation is therefore not a class of bug here —
it is handled once, in the one place that needs contiguity. The bitmap itself
comes out of a static array because it cannot be allocated by the allocator it
describes.

## 2026-10-03 — `PTE_CACHE_*` is left as it is until something depends on it
Decision: the cacheability flags keep using bits 6:7 (the PAT field) rather than
the PWT/PCD/PS combination the SDM describes, and the framebuffer window stays
write-combining while MMIO windows are nominally "uncacheable".
Alternatives: fixing it now.
Reason: it is wrong but nothing observes the difference yet, and phase 3's real
work was elsewhere. It is recorded in STATUS.md as a known gap rather than
quietly left, so it does not become folklore.

## 2026-10-03 — Per-CPU state through %gs, with the pointer in the first word
Decision: each per-CPU slot's first member is a pointer to its own state, and
`percpu_self()` is a single `movq %gs:0`.
Alternatives: indexing the array by a processor number in a register (which
needs a `swappergs`-style scheme to find the number first); an MSR per field; a
segment-relative offset per member, which hard-codes every field's offset into the
assembly that touches it.
Reason: the accessor is what has to be inlinable, because it is called from
interrupt handlers that are counting ticks. A "read the processor number, then
index" accessor needs a register the caller may already be using, and a
per-member GS offset turns every `percpu_t` change into an assembly change. One
indirection through the first word keeps the struct a plain C struct and the
accessor one instruction. The cost is that the accessor cannot find its own
answer, so `percpu_self_index()` is the one lookup that compares a pointer against
the array instead.

## 2026-10-03 — The AP trampoline is our own, not Limine's handoff
Decision: `kernel/arch/x86_64/smp_trampoline.S` takes an application processor
from real mode to the kernel, and the kernel sends the startup IPIs itself.
Alternatives: using Limine's per-CPU `goto_address`; mapping the reset vector to
the kernel's own wait stub; writing the reset vector page directly.
Reason: the kernel owns its page tables after the CR3 switch and Limine's
`goto_address` is a boot-time handoff into a world this kernel has already
replaced, so its preconditions no longer hold. Writing a wait-for-SIPI loop into
the physical page at `0xfffffff0` would work on this machine, but that page is the
firmware's and is not in the memory map as usable. Mapping `0xfffff000` in the
kernel's own tables changes nothing, because a released processor is in real mode
with paging off and never consults them -- which is the non-obvious part, and the
reason the obvious fix is not a fix.

## 2026-10-03 — The INIT half of INIT/SIPI is not sent yet
Decision: `smp_start_all()` sends the two startup IPIs without the preceding INIT,
and reports a processor that does not come up rather than taking the machine down
to find out.
Alternatives: sending a targeted INIT (which resets the sender too); sending the
"all excluding self" INIT (which releases the processors straight into whatever
the bootloader's code is at the reset vector); failing the self-test when the
count is short.
Reason: an INIT is a level interrupt whose destination field is ignored, so it
resets every processor including the sender. Releasing a processor sends it to
`0xfffffff0`, where hardware holds the firmware's wait-for-startup loop and this
machine holds the bootloader's code; the released processor executes it, faults
with no IDT of its own, and triple-faults the machine. An incomplete phase that
still boots and says what it could not do is more useful than a red build that
says nothing about why.

## 2026-10-04 — The bootloader starts the application processors; the kernel only hands them an entry point
Decision: `smp_start_all()` does not send INIT or a startup IPI. It writes
`mp_info->reserved` (the stack), `mp_info->extra_argument` (a direct-map address
holding the CR3, CR4, slot index and IST stacks) and `mp_info->goto_address` (the
entry point), and the processor jumps there. `smp_start_cpu()` and
`smp_trampoline.S` are kept for a bootloader that does not do this, and are not
called.
Alternatives: INIT/SIPI by hand; writing a wait-for-startup loop into the physical
page at `0xfffffff0`; mapping `0xfffff000` in the kernel's own tables.
Reason: the bootloader starts every application processor itself, out of its own
INIT/SIPI sequence, and parks each one spinning on `mp_info->goto_address` — that
is what it does with them, and the measurement that settled it was reading its
source, not more guessing at the machine. INIT by hand cannot work here: INIT is a
level interrupt whose destination field is ignored, so it resets the sender too,
and the released processor fetches from the reset vector, where this machine holds
the bootloader's code rather than firmware's wait loop. It executes that, faults
with no IDT of its own and triple-faults. Mapping `0xfffff000` changes nothing for
the same reason the other option fails: a released processor is in real mode with
paging off and never consults page tables.

The hand-off has a consequence worth stating separately: a parked processor is
running on the *bootloader's* page tables, so the frames behind them are still in
use after the kernel has switched CR3. `vmm_release_boot_tables()` therefore waits
for the hand-off to complete before the allocator gets them back. Releasing at the
CR3 switch, as before, would hand three processors' page tables to the heap.

## 2026-10-04 — One TSS, one descriptor and one IST stack per processor
Decision: `kernel_tss` is an array indexed by processor index, each processor has
its own TSS descriptor slot at `GDT_TSS + index * 16`, and its own IST 1 and IST 2
stacks from the heap.
Alternatives: one shared TSS and one shared descriptor (what it was); a per-CPU copy
of the whole GDT.
Reason: loading a task register sets the descriptor's *busy* bit in the table it
came from, so the first processor to run `ltr` marks the entry and every later
processor is refused with #GP and the selector as the error code. This was the
last thing standing between the hand-off and four live processors, and it looks
exactly like a corrupt descriptor if read without knowing about the busy bit. The
IST stacks cannot be shared either: an IST pointer is an absolute address the CPU
puts on the stack without involving the fault handler, so a shared stack means one
processor's double fault landing in the middle of another's.

## 2026-10-04 — The descriptor table is reloaded, not just the segments
Decision: `gdt_reload()` does the `lgdt` as well as the segment and task-register
reloads, and `gdt_reload_for(index)` additionally loads that processor's TSS.
Alternatives: leaving `lgdt` out and relying on the boot processor having loaded
the table, which is all that was needed before phase 4.
Reason: a processor arriving from the bootloader's hand-off is still using the
bootloader's descriptor table. Its far return to selector 0x08 finds whatever the
bootloader's table has at that index, which here is a 32-bit code segment, so the
processor drops into compatibility mode on the first instruction after the reload
and faults on the next fetch — with no IDT loaded yet, which turns one bad fetch
into a triple fault. Reloading the table is the only part that cannot be skipped.

## 2026-10-04 — LTR needs an explicit operand-size prefix, and the TSS offsets are not optional
Decision: `ltr` is written as `.byte 0x66, 0x0f, 0x00, 0xd8`, and the TSS struct
declares the reserved word at 0x04 so RSP0 lands at 0x08 and IST 1 at 0x28.
Alternatives: `ltr %ax`; leaving the TSS four bytes shorter.
Reason: neither assembler honours an operand-size suffix on LTR, so `ltr %ax`
assembles to the 32-bit form, which is undefined and raises #GP with the selector
as the error code. The boot processor never noticed because the compiler folded
the constant there and emitted the right encoding by hand. The TSS layout was the
same kind of mistake: the four bytes at 0x04 look spare — that is where a 32-bit
TSS keeps SS:0 — but the CPU reads the real offsets, so RSP0 was being read as
zero and the IST entries from eight bytes too low. Neither shows up until
something takes a ring transition or a double fault.
## 2026-10-04 — Every local APIC handler ends the interrupt, including the spurious one
Decision: `smp_ipi_common()` writes the end-of-interrupt register, and so does the
spurious-vector path in `isr_dispatch()`.
Alternatives: leaving both to the hardware; an end-of-interrupt in one central
place at the end of `isr_dispatch()`.
Reason: the interrupt-acknowledge sequence sets an in-service bit, and on xAPIC
nothing else clears it. Miss it and the processor refuses every later interrupt of
the same priority — and for the local APIC that is all of them, since fixed vectors
share a priority class. The processor does not crash and reports nothing: it stops
being interrupted, its idle loop's `hlt` stops returning because the request
register still holds something, and every IPI sent to it is silently dropped.
Measured on this machine: an application processor took exactly one IPI and then
went permanently deaf, which looked like an IPI that was never sent.

The spurious vector is the subtle one. It sets an in-service bit like any other
interrupt, and it is the only kind whose handler has to clear that bit explicitly,
because the acknowledge sequence an interrupt gate issues is suppressed for it:
there was nothing to acknowledge in the first place. It is also the kind that turns
up spontaneously, so a kernel that skips it is a kernel that works until a
processor has been running long enough to see one.

## 2026-10-04 — The MCS unlock decides with a compare-and-exchange, not an exchange
Decision: `mcs_unlock()` uses a compare-and-exchange on the queue tail, succeeding
only if the tail is still this node.
Alternatives: an unconditional exchange that leaves the tail empty; a compare and
swap on the node's own `next`.
Reason: an exchange clobbers the tail to empty even when a successor is already
queued behind it, and the next processor to arrive finds an empty queue, concludes
the lock is free, and walks into the critical section the successor is about to
enter. Two processors inside at once, so updates are lost, and a queue that has
lost its own tail, so the waiters behind them never get a hand-off. It passes on
two processors and fails on three, which is the only reason it was ever found.

## 2026-10-04 — Contended locks are checked against a counter, not a flag
Decision: every lock self-test is a shared counter that must end up with exactly
the sum of every processor's work, run on every online processor at once, on 1, 2,
3, 4 and 8 processors.
Alternatives: a test-and-set lock that would be checked for exclusion; timing the
lock and asserting it is "fast enough".
Reason: mutual exclusion has no positive test. A flag says nothing — a lock that
lets two processors in and a lock that works can both leave any flag in any state,
and the interesting failures are lost updates, which only a shared counter shows.
And "fast enough" is a threshold that passes on two processors and hides exactly
the starvation the ticket lock exists to prevent, so fairness gets its own check:
every processor must be served every round.

## 2026-10-04 — The scheduler's only lock is the run queue, and it is never held across a switch
Decision: one run queue per processor, each with a ticket lock. `schedule()` takes
the lock, picks a task, drops the lock, and only then switches. The context switch
itself takes no locks at all. Nothing is ever *pushed* between processors: a
processor whose queue is empty goes and takes a task from another's.
Alternatives: one global run queue; a big kernel lock around the scheduler; periodic
load balancing.
Reason: this is the whole difference from the version with a big lock. No lock is
held across a switch, none is held across anything that could block, and the order
between two run queues is never a question — there is no order, because there is
only ever one lock in hand. The cost is real and is paid knowingly: a task chosen for
a processor can be taken by another one between dropping the queue lock and switching
to it, so `state` is the arbiter, only one processor ever wins the
`TASK_RUNNABLE → TASK_RUNNING` transition, and the loser gives the task back.

Three things about it are not obvious and each one cost a hang:

- **A guard on the switch may not be left set across it.** A per-processor
  "already switching" flag that is cleared only after `task_switch` returns stops
  the *next* task on that processor from ever sleeping, because it finds the
  processor mid-switch for as long as the previous task is away. It has to be
  cleared just before the switch, with interrupts off, which is safe precisely
  because nothing can arrive in those few instructions.
- **A task switched to must not inherit the switcher's interrupt state.** The
  commonest route to `schedule()` is the timer handler, where the interrupt gate
  has just cleared IF, and a task with no saved context inherits whatever the
  processor happened to have. For an idle task that is fatal rather than merely
  wrong: its first act is `hlt`, and `hlt` with interrupts disabled never wakes. The
  processor then sits in its idle loop with its timer still running and takes no
  further interrupts — which looks exactly like a scheduler whose timer has stopped,
  and cost an hour to read as such.
- **Interrupts must be off across "decide to block" *and* the switch.** With them
  on, a tick can preempt a task between putting it on the sleep list and switching
  away; since it is deliberately not put back on the run queue, it is then on the
  sleep list and running nowhere, and the next wake has it linked into two lists at
  once.

## 2026-10-04 — A task that is in two lists needs two pairs of links
Decision: `task_t` has `next`/`prev` for the transient lists (a run queue or the
blocked list, never both) with a `list_tag` saying which, and a separate `all_next`
for the permanent list of every task.
Alternatives: one pair of links, with the task list rebuilt on demand; a hash or
array indexed by task id instead of a list.
Reason: the list of every task is permanent — a task is in it from creation until
the machine stops — while the run-queue links are rewritten every time it is queued.
Sharing one pair is not a shortcut that happens to work. Registering a task walks to
the tail of that list and follows the tail's links to do it, so queueing a task
overwrites the links the task list is threaded through, the *next* task registered
walks into the run queue instead and is appended to it, and the queue's length stops
matching what it holds. The symptom is not memory corruption in the usual sense: a
queue whose length reads 4294967295 and a test that times out because a worker's
task never ran.

`list_tag` and the checks in `rq_push()`/`rq_remove()` are what turned that from a
hang into a one-line panic naming the task and the list.

## 2026-10-04 — `isr_common` may rely on `%rbp` surviving the handler, and `irq_restore` may keep its `popfq`
Decision: `isr_common` keeps `movq %rbp, %rsp` to recover its register frame, and
`irq_restore()` keeps restoring the whole saved RFLAGS word with `popfq`. Both are
things that look like bugs, and both were "fixed" while chasing the phase-5
multi-processor fault; both fixes were reverted because each one turned a merely
intermittent fault into a deterministic hang at exactly the same place.
Alternatives: save the frame address on the stack and restore `%rsp` from it; rebuild
the restore as `sti`/`cli` on the interrupt-enable bit alone.
Reason: **`%rbp` is callee-saved.** The kernel is built without
`-fno-omit-frame-pointer`, so a function may use `%rbp` as a general register — but
any function that modifies it must restore it, so it survives the handler chain
exactly as the ABI says, including a handler that switches stacks. The theory that it
does not was attractive because it explained a faulting `rip` inside the data
segment, and it was wrong. Likewise a whole-word `popfq` can in principle restore a
stray trap or direction flag, but replacing it with the interrupt-enable bit alone
hung the guest reproducibly where the original did not, and an unverified change is
worse than a documented sharp edge.

Both are recorded here rather than fixed, because the fault they were invented for
is still open (see `docs/STATUS.md`) and a fix that was not the cause is a fix
nobody should keep.

## 2026-10-05 — Each ISO stages into its own directory
Decision: `build_iso` stages into `build/isostage/<iso-name>/`, named after the ISO
being built, rather than into one shared `build/isodir`.
Alternatives: `.NOTPARALLEL:` on the two ISO rules; serialise the ISO builds with an
order-only prerequisite; build only one ISO at a time.
Reason: `make -j8 iso iso-test` builds both at once, and both rules began by
`rm -rf`-ing the same directory and then wrote their own `boot/limine.conf` into the
same path. Which ISO came out right depended on the order the two `cp`s happened to
land in.

The failure is silent and it lies. The test ISO ends up carrying the *run* ISO's
`limine.conf`, so `minx.test=1` is never passed, the guest boots to the interactive
console instead of running the self tests, and `make test` reports "the guest never
reached TEST-READY" until the harness times out — which reads exactly like a kernel
that ignores its command line, and cost a long time being believed. A per-target
staging directory is also the right thing under `-j` generally: the two builds then
share nothing but their prerequisites.

`.NOTPARALLEL:` would have hidden it rather than fixed it — the two ISOs genuinely do
not interfere once each has its own directory.

## 2026-10-05 — The one division on the preemption path is guarded
Decision: `charge_current()` refuses to divide unless the current task's weight is in
range, and `panic()`s naming the processor and the pointer if it is not.
Alternatives: leave it and report "unhandled CPU exception: vector 0"; make weight a
power of two and shift instead of dividing.
Reason: `elapsed / weight` is the only division on the preemption path, and a
division by zero is not a wrong answer, it is an exception. The kernel's own handler
reported it as "unhandled CPU exception: vector 0" and pointed at `charge_current`
with nothing indicating what was wrong, which is the least informative panic message
the kernel can produce. With the guard the same run says
`sched: processor 2 has 0xffff800000100030 as its current task, and that has weight 0`
— which is the whole bug, on one line.

A weight that is zero, or above `TASK_WEIGHT_MAX`, cannot come from any path that
creates a task, so the check is not papering over a case the scheduler should allow.
It converts a fault with no explanation into a report naming what is wrong.

## 2026-10-05 — The syscall door is `int $0x80`, not `syscall`
Decision: a program reaches the kernel through vector 128 with `int $0x80`, through
the ordinary interrupt path, and returns with `iretq`.
Alternatives: `syscall`/`sysretq` with STAR, LSTAR and SFMASK, which is the better
instruction and is where this should end up.
Reason: `syscall` derives its CS and SS from STAR[63:48] + 16 and + 8, and `sysretq`
derives them from R11[47:32] and + 8. That forces the code and data selectors into a
particular order in the GDT in *both* directions: kernel data eight below kernel code,
and user code eight below user data. This kernel's GDT has the usual order — kernel
code below kernel data, user data below user code — so making `syscall` work means
reordering the GDT around an instruction rather than around a layout that other code
already assumes.

`int $0x80` costs one interrupt instead of one instruction and gets the TSS's RSP0
stack switch for free, which is exactly what a syscall needs and which `syscall` does
not do at all. Reusing the existing stub, dispatcher and frame means the privilege
transition cannot get subtly wrong in a way that has no test, and the return is
`iretq`, which is the only return a `regs_t` frame can express.

Revisiting this means reordering the GDT, which is a real piece of work: the descriptors
are built in `gdt_init()` and the TSS descriptors are per processor and indexed
alongside them. It is the right change and it is not this phase's.

## 2026-10-05 — A program's pointers are checked by walking, not by faulting
Decision: `syscall_user_range_ok()` walks the running task's own page tables and checks
every page of a range — present, user-accessible, and writable for a write — before
anything dereferences it.
Alternatives: fault and recover, catching the fault in the handler and re-running the
copy; a per-address-space region registry to check ranges against instead of tables.
Reason: a walk is exact. A fault-and-recover needs the fault handler to distinguish "this
fault was mine, on purpose, on an address I was told to touch" from "a bug in the kernel",
which is a distinction every future fault handler has to make correctly and forever; and
it has to re-run the faulting instruction, which for a string copy is a partially
completed one. The walk is ~10 instructions per page, needs no state, and a range it
rejects is a range that would have faulted.

This is also why the boundary has no `stac`/`clac` pair in it. Walking the tables does
not dereference the user address at all, so the one thing SMAP exists to prevent cannot
happen here — where a syscall actually *writes* to a user buffer is `writev`, and that
one does need it, and does it later.

## 2026-10-05 — Vectors a ring-3 task can receive are opened to DPL 3
Decision: the local APIC timer gate and the reschedule and wake IPI gates are DPL 3. The
exception gates stay DPL 0.
Alternatives: DPL 3 for everything above 32; a separate "return to user" trampoline per
vector; leaving them DPL 0 and hoping the timer never fires.
Reason: a gate's DPL is checked against the privilege level of the *interrupting* code.
The timer is what catches this out — one millisecond after a ring-3 task starts, the
LAPIC timer fires, and through a DPL-0 gate that is a #GP, which kills the program and
reads as a loader bug rather than as a privilege problem. The two IPIs are the ones a
scheduler sends to a processor that may be running user code.

Exceptions are deliberately left closed. The CPU delivers an exception through a DPL-0
gate *even when it came from ring 3*, so a user fault still gets to its handler; what DPL
0 prevents is user code raising a chosen *vector* on demand with a frame it controls.
That is a real protection, and the syscall door is reachable by `int` because `int`
deliberately does check DPL — so the syscall gate is opened on purpose and everything
else is not.

Opening the timer gate means user code can `int $0x30` deliberately. Both handlers are
safe against a frame it controls: the tick handler counts, charges the current task and
preempts it, and the reschedule handler drains a per-CPU work slot, and all of that is the
kernel's own state rather than anything read out of the frame.

## 2026-10-05 — CR4.SMAP is left off, on purpose and with a count
Decision: `pt_write_cr4()` does not set SMAP. Kernel code builds user page tables and
writes into user pages through the direct map.
Alternatives: set SMAP now and put `stac`/`clac` around the three places that need it.
Reason: SMAP turns every kernel write to a user page from "works" into "faults", and
there are exactly three such places — the loader's segment copies, its initial stack, and
`task_create_user()`'s return frame. Each would need an `stac`/`clac` pair and a comment
explaining why, and the thing SMAP protects against is already handled better: a program's
pointer never reaches the kernel unchecked, because `syscall_user_range_ok()` refuses it
before it is dereferenced.

A short list is the argument for turning it off now. The list gets longer as phase 7 adds
COW, a real heap and a growing stack, and at that point the pairs outnumber the
protections. The decision to revisit it is written down where the three sites are.

## 2026-10-05 — Tried and reverted: finishing a task's context before publishing it
Decision: reverted. The kernel is at the state before the attempt: `schedule()` requeues the
old task and *then* switches, as before.
Alternatives kept on record, none of them implemented.
Reason, and it is the most important thing found about the phase-5 fault: **a preempted task
must not be claimable before its context is finished.** `schedule()` publishes the old task
to a run queue before switching away, and between the two there is a window -- a lock
acquisition, an address-space load -- in which the old task's saved stack pointer still
refers to a frame on the preempting processor's stack. A second processor that claims it
there switches to a half-written context and runs it with the preemptor's registers, which
is how a `task_t *` becomes a kernel text address.

The correct fix is to save the context, *then* publish, *then* restore. That sounds like a
reordering and is not: a stack-based saved context is not finished until the instant it is
restored from, and the scheduler must do work in between, so the context has to move out of
the stack and into `task_t`. `task_t.regs[8]` -- six callee-saved registers, a return address
and a stack pointer -- is where it goes, and `schedule()` then orders every early return
*before* the save, because after the save there is nothing on the stack for a normal return
to unwind.

The attempt is kept in `git stash` rather than deleted, and `docs/STATUS.md` lists the three
mistakes in it: publishing with a non-claimable `TASK_SWITCHING` state deadlocks the
scheduler; a save half that returns leaves the registers below the stack pointer where the
bookkeeping overwrites them; and a never-run task needs both context forms populated, or a
restore loads `RSP = 0` and the machine triple-faults on the first instruction.

Reverted rather than committed because it did not boot. A fix for the root cause of a fault
that only moves the fault somewhere else is not a fix, and the tree is better off working
and documented than broken and interesting.

## 2026-10-05 — Fixing the phase-5 fault for real: save-and-publish inside one call
Decision: implemented. Supersedes the entry above, which was right about the cause and
wrong about the remedy.

The cause stands: **a preempted task must not be claimable before its context is
finished.** `schedule()` used to requeue the outgoing task and *then* call
`task_switch()`, which writes the outgoing task's `saved_rsp` as its last act. In
between sat the run-queue lock and the address-space load, so for that whole window the
task was reachable by another processor while its saved stack pointer still named an
older frame. That processor restored a stale `RSP`, and the task resumed executing a
stack frame already in use. Two live frames on one kernel stack is a hang, and it
presents as a processor whose timer has stopped — which is exactly how the phase-5 fault
looked, and why it took so long to find.

The remedy above was to move the context out of the stack into `task_t.regs[8]`, so it
could be finished before publication. That attempt did not boot, and the diagnosis in
`docs/STATUS.md` is right: three separate mistakes, each of which turned an
intermittent fault into a deterministic one. It is kept in `git stash` rather than
deleted. But the move to `regs[]` was not necessary, and paying for it was what made
the attempt fragile.

**The context can be finished on the stack; what it cannot do is skip the code between
the save and the publication.** Both halves of `task_switch()` were indivisible only
because they were written as one function, and the ordering problem lived in C, not in
the assembly. So `schedule()` now uses two halves, with the publication inside the save:

    task_save_and_publish(&prev->saved_rsp, prev, c);

`task_save_and_publish()` in `switch.S` pushes the six callee-saved registers, stores
`%rsp`, *then* calls `sched_publish_outgoing()` in C, then returns. Three properties
fall out of that order, and all three are load-bearing:

- The registers are on the stack and `saved_rsp` is final **before** the task is
  reachable by another processor. There is no window.
- The resume point — where that function's `ret` lands in C — is **after** the queue
  insertion. Saving first and publishing from C afterwards would look equivalent and is
  not: the resume point would be *before* the insertion, so every preemption would
  republish the task, putting one task on one list many times over.
- The callback's own frame sits below the saved context, in space the next save reuses.
  That is why it must touch nothing that outlives it. It takes the three arguments the
  assembly already has in place, so no register shuffling is needed.

`task_restore_context()` is the load half, and deliberately has no `sti`: a task that
has already run has live state to return into, and that state decides whether interrupts
should be on. A task that has never run still goes through `task_enter_first()`, which
turns them on. This is the invariant that was previously only asserted in a comment and
was in fact untrue — `task_switch()` never saved `RFLAGS`, so a resumed task got the
*switcher's* interrupt state and relied on the `irq_restore()` after the switch call to
put its own back. That works on both paths that exist today and is left alone, because
the fault in hand is not it, and changing the frame layout would put the ring-3 entry
frame in `task_create_user()` and `task_enter_user()` at risk for no evidence.

`task_switch()` is retained unchanged. It remains the correct primitive for a switch that
cannot race, and nothing was removed to make room for the split.

Rejection logged as well: moving the context into `task_t` is *not* what fixes this, and
the three mistakes it took are now written down in `docs/STATUS.md` so the next attempt at
the same idea does not repeat them.

### The saved-frame layout, written out because it is easy to get wrong

With `save_rsp` = `%rsp` after the six pushes (S-56, where S is `schedule()`'s stack
pointer and the `call` put its return address at S-8):

    S-16  rbp     \
    S-24  rbx      |
    S-32  r12      |  the callee-saved set, as task_restore_context's pops read it
    S-40  r13      |
    S-48  r14      |
    S-56  r15     /   <- saved_rsp points here
    S-64  return address of `call sched_publish_outgoing`  (scratch: reused next time)

`task_restore_context()` pops upward from `saved_rsp` and its final `ret` takes the word
at S-8, which is the return address of `call task_save_and_publish` — so a resumed task
continues in `schedule()` *after* the save-and-publish, which is after the queue
insertion. That is the property the whole design exists to get.

`task_save_and_publish()` therefore needs **two** `ret`s, not one: the callback's `call`
pushed a return address below the saved registers, and the saved context is six quadwords
above where that address is. One `ret` unwinds the callback and then falls through into
the saved `r15` and executes it — which kills the machine before the first processor has
finished announcing itself. Cost: one build-and-run cycle to find, and it is exactly the
kind of thing that reads as a scheduler bug in the log.

## 2026-10-05 — Six faults in the userspace path, all the same mistake in five places
Decision: implemented, with one thing deliberately left undone.

The recurring mistake is now identifiable, and naming it is worth more than any single
fix: **"map or read something in an address space, using the running task's tables."**
`vmm_tables()` is the running task's, and that is the right answer almost everywhere --
it is the reason nothing has to pass an address space around. It is the wrong answer
exactly when the thing being mapped or read belongs to a task that is not running yet,
or is not the running task. Five places did it:

- `task_create_user()` mapped the new task's stack through `vmm_map_user()`, so the stack
  went into the creator's address space. The task was created with a stack nothing could
  reach: its own tables had no entry for the address, and the walk over those tables
  correctly said nothing was mapped there -- so the first access faulted while the check
  said everything was fine.
- `elf.c`'s `load_segment()` did the same with the program's image, so a program was
  loaded with no code at its entry point.
- `elf.c`'s `build_stack()` computed its kernel alias with `virt_to_phys()` applied to a
  *user* address. That subtracts the direct-map offset, so it wraps, and `phys_to_virt()`
  of the result lands in the hole just below the kernel image. The argv/envp/auxv block
  was written there, the real stack was never zeroed, and `entry_rsp` handed to `iretq`
  was a direct-map address.
- `elf.c`'s `build_stack()` also produced `entry_rsp` and `argv0_addr` in the kernel
  alias. The program was entered with a stack pointer into the direct map -- which
  `iretq` rejects with a #GP -- and with `argv[0]` pointing into the kernel.
- `elf.c`'s program-header fallback asked `vmm_translate()` and
  `syscall_user_range_ok()` whether the headers were mapped, i.e. asked the *loader's*
  tables, always got "not mapped", and went on to map over a page the first segment had
  already placed.

The fix is additive rather than a change of convention: `vmm_map_into()`,
`vmm_translate_in()` and `syscall_user_range_ok_in()` take the address space, and
`vmm_map_user()`/`vmm_translate()`/`syscall_user_range_ok()` are now one-line wrappers
around them that pass the running task's. Nothing that was right before is different, and
each of the five call sites says why it needs the explicit one.

Three more, which are not that mistake but are worth recording because each produced a
symptom that pointed somewhere else entirely:

- **The ring-3 return frame was written backwards.** `iretq` pops RIP, CS, RFLAGS, SS and
  RSP, each one quadword above the last. `task_create_user()` wrote RFLAGS, CS, RIP, SS,
  RSP and compensated in `task_enter_user()` by advancing %rsp to reach RIP in the middle
  of the frame. That put RIP in the right slot by coincidence and left CS holding the SS
  selector, with SS and RSP read from above the frame.
- **The selectors had RPL 0.** `iretq` takes the new CPL from the RPL of CS, so a frame
  built with `0x20`/`0x18` does not drop privilege at all: it pops RIP, CS and RFLAGS,
  stops, and leaves SS and RSP on the stack. The task ran its first instruction in ring 0.
  `GDT_USER_CODE_RPL3` and `GDT_USER_DATA_RPL3` are the same descriptors at RPL 3; the
  index used to reach them is `selector >> 3`, so the RPL does not change which entry it is.
- **The loader's initial stack block was written over both frames.** `build_stack()` lays
  out argc/argv/envp/auxv and the argument strings from the top of the stack down, and
  `task_create_user()` had put the saved context and the ring-3 frame at the very top.
  The two layouts overlapped completely: the strings landed at `top-16` and the auxiliary
  vector at `top-144..top-16`, over the saved context and over CS, RFLAGS and SS. Only RIP
  and RSP were rewritten afterwards, so the frame came out half right -- the right entry
  point with a CS selector built out of two words of auxiliary vector. The frames now own
  the top of the stack and the program's region is everything below them.

And one design change that the last of those made unavoidable:

**A ring-3 task's context is not a set of callee-saved registers and a return address.**
A task interrupted in ring 3 has a full return frame -- RIP, CS, RFLAGS, SS, RSP --
pushed by the CPU above where the handler runs, because the CPU switched to a kernel stack
first. Resuming that with `ret` pops the ring-3 RIP as a return address and jumps to it in
ring 0 with the kernel's stack and CS, discarding everything the frame said about SS and
RSP. So `task_save_publish_and_switch()` records whether the outgoing task was in ring 3 --
by reading `%cs`, which C cannot do -- and the restore ends in `iretq` or `ret` accordingly.
`iretq` pops from exactly where `ret` would have taken the first word, so the two cases
differ by one instruction.

That was hidden by `frame[2] = 0x002`, which read as though it enabled interrupts and
actually did not: 0x002 is only the reserved bit the CPU forces to one, and IF is bit 2. So
ring-3 tasks were never preempted, and a task that is never interrupted in ring 3 never has
a ring-3 context to save. It is 0x202 now, which means the path is actually exercised.

Two costs of this worth stating plainly:

- The flag pointer cannot live in `%r15` past the restore, because the first `popq %r15` is
  the *chosen* task's %r15 and is zero for a task that has never run. It moves to `%r11`,
  which the pops do not touch. Getting this wrong faults on the very first switch on the
  machine, which is at least a memorable symptom.
- `task_save_publish_and_switch()` now takes seven arguments, so the seventh is on the stack
  and has to be read before anything moves `%rsp`. Both flags are needed -- the save writes
  the outgoing task's, the restore reads the incoming task's -- and passing only one
  compiles, links, boots, and then reads the caller's return address as a pointer.

**Left undone, deliberately:** a user task still has no kernel stack. The kernel runs on
the user stack for it, and `TSS.RSP0` is its processor's boot stack. That works -- the CPU
switches to RSP0 on the way in from ring 3 -- but the frames are unguarded, not per-task,
and every stack trace of a fault inside a syscall is meaningless. It is recorded in
`docs/STATUS.md` as the first thing phase 6 still needs, not hidden.

## 2026-10-06 — The ring-3 return frame had SS and RSP the wrong way round
Decision: implemented. This was the phase-6 defect, and it was there from the first version
of the frame.

`iretq` pops RIP, CS, RFLAGS, **RSP, SS** — RSP fourth, SS fifth. The frame was built
RIP, CS, RFLAGS, SS, RSP. And an earlier version of the *same* frame was also the wrong way
round (RFLAGS first, RIP last), compensated in `task_enter_user()` by advancing `%rsp`; the
two bugs together meant the frame had been wrong twice, in both directions, and each time it
looked like something else.

**Why it cost so long, which is the part worth keeping.** Reading a stack pointer where a
selector belongs raises `#GP` whose error code is the low half of that stack pointer. So
the report was `error code 0xef00` — an error code that looks like a selector and is not
one, and that points at the frame rather than at anything the CPU did with it. Two
measurements settled it in one run each:

- Changing the word the author believed was RSP from `0x7fffffffef00` to `0x7fffffffcf00`
  changed the error code from `0xef00` to `0xcf00`. So that word was being read as
  something whose value appears in the error code — and SS is exactly that.
- Changing the word the author believed was SS changed the error code not at all.

A third measurement had already shown the `%cs` question was a dead end: recording `%cs`,
`%rsp` and all five quadwords from `__enter_user` itself, immediately before the `iretq`,
gave `cs=0x8 rsp=0x7fffffffefd8 | rip=0x40042c cs=0x23 rflags=0x202 ss=0x1b
rsp=0x7fffffffef00` — every value correct and `%rsp` exactly at the frame. That is what
made it certain the frame was not *placed* wrongly and only *popped* wrongly, which is the
distinction the two experiments above then confirmed.

**Lesson, recorded because it will recur:** a diagnostic that reads the frame through the
kernel's alias proves the frame was *written* correctly. It says nothing about whether
`%rsp` points *at* that alias when the CPU gets there, and those were different here. The
measurement that answered the question cost five stores and a five-iteration loop in the
ring-3 entry path, and it is kept: `task_iret_frame[]`, printed by `isr_report_exception()`.

Rejected along the way, and recorded so it is not repeated: deriving the ring-3 context flag
from `%cs` in `task_save_publish_and_switch()` (an interrupt from ring 3 switches the CPU
to CPL 0 *before* the handler runs, so `%cs` is the kernel's and cannot see the frame —
`isr_dispatch()` records it now), and running C to print from `__enter_user` (that runs on
the task's user stack, so instrumenting the entry that way changes what it measures).


## 2026-10-06 — Two things a test can be wrong about
Decision: both fixed, and both recorded because the second is the more useful lesson.

**A TSS descriptor's base is a physical address, and `phase4-tss` asserted the virtual
one.** The CPU fetches TSS contents from that field directly -- the TSS is not subject to
paging, not even in long mode -- so the boot processor's `RSP0`, IST pointers and
`iomap_base` were being read out of physical `0xffffffff80xxxxxx`, which is not memory.
Nothing about that shows up until something switches stacks, which is why it survived the
whole of phases 1 to 5: an exception at CPL 0 needs no stack switch, so every test passed
and the first ring-3 fault loaded a garbage `RSP0` and died.

The test agreed with the bug. It compared the descriptor's base against
`(uint64_t)&kernel_tss[i]`, which is exactly what a kernel with this bug would contain --
so it was not a weak test, it was a test that asserted the wrong thing, and it would have
kept passing for as long as the bug existed. **A test that verifies a structure's contents
without ever asking the hardware what it thinks those contents are is not testing the
integration.** It now compares against `virt_to_phys()`.

**And the boot processor's descriptor was written before `virt_to_phys()` meant anything.**
`gdt_init()` runs first in `kmain()` -- deliberately, so that a fault is reportable from the
first instruction -- and it has to write that descriptor, but `hhdm_init()` has not run yet,
so `virt_to_phys()` is the identity and the descriptor gets the virtual address again. Fixed
by re-installing the descriptor immediately after `hhdm_init()`. Moving `gdt_init()` later
would have been the other answer and is worse: it delays the descriptor tables, which exist
precisely so that a fault is reportable, for no gain.

The lesson generalises to the whole of this session: three separate bugs (the ring-3 frame
written backwards, the frame's SS and RSP swapped, the TSS base wrong) all presented as
*the hardware disagreeing with the kernel*, and in each case the kernel was right to believe
itself and the hardware was being asked the wrong question -- or the question was right and
the answer was not being read. `docs/STATUS.md` records, for each, the measurement that
settled it rather than the argument that did not.

---

## 2026-10-06: the 64-bit TSS has one reserved quadword, not two, and its stack pointers start at 0x04

`tss_t` carried `reserved0` and `reserved1`, so every field after the top sat four bytes
above where the processor reads it. RSP0 came out of the reserved word (zero), IST1 came
out of `reserved2` (zero), and `iomap_base` landed on a reserved word — which still means
"no I/O bitmap", so the test that checks it passed.

The offsets are `reserved0` 0x00, `rsp[0..2]` 0x04/0x0C/0x14, `reserved2` 0x1C,
`ist[0..6]` 0x24..0x54, `reserved3` 0x5C, `reserved4` 0x64, `iomap_base` 0x66. The size is
0x68.

**Where the offsets came from.** Two independent implementations, both read rather than
recalled:

- QEMU's `get_rsp_from_tss()` computes the offset as `8 * level + 4`, and is called with
  `level = dpl` for a privilege change and `level = ist + 3` for an IST — which puts IST1
  at 0x24. The same 4-byte-short shape appears in `switch_tss_ra()`'s 32-bit branch.
- Linux's `struct x86_hw_tss` is `{ u32 reserved1; u64 sp0; u64 sp1; u64 sp2; u64
  reserved2; u64 ist[7]; u32 reserved3; u32 reserved4; u16 reserved5; u16
  io_bitmap_base; } __attribute__((packed))`, so `sp0` is at offset 4 and
  `io_bitmap_base` at 0x66 — and Linux takes a ring-3 stack switch on every syscall on
  every machine anyone runs it on.

The 64-bit TSS is the 32-bit TSS's `esp0` slot widened to 64 bits: it keeps the 32-bit
layout's `esp0` position and spends the next eight bytes on the ring-1 pointer that the
32-bit layout spends on `ss0`. Adding a second reserved dword "for symmetry" is exactly
the mistake the comment in `kernel/include/cpu.h` now warns about.

**Why nothing noticed for five phases.** An exception at CPL 0 needs no stack switch — the
CPU pushes onto the stack it is already on — so every fault test from phase 1 onward
passes with a completely unusable TSS. Nothing about the ring-3 boundary works until
something switches stacks, and `phase6-user`'s first syscall is the first thing that does.

**The test is the other half of this.** `phase4-tss` used to check `reserved1 == 0 &&
reserved2 == 0`, which a struct full of zeros satisfies however it is laid out. It now
asserts `offsetof(tss_t, rsp[0]) == 0x04` and friends. The processor reads a byte at a
fixed offset; asserting a field by name asks a different question, and this one had the
same answer either way.

## 2026-10-06: the TSS descriptor's base is a physical address, and it has to be mapped there too

A TSS descriptor's base is fetched by physical address; the TSS is not subject to paging
even in long mode. Two things follow, and both bit.

**`virt_to_phys()` is not the conversion.** It subtracts the direct-map offset and is only
correct for an address *in* the direct map. The kernel image is not in the direct map — it
is mapped once, at `__kernel_start`, from whatever physical base the bootloader chose — so
converting an image address that way gives `0x00008000xxxx_xxxx` for `0xffffffff80xxxxxx`,
which reads like a plausible low physical address and has nothing behind it.
`vmm_kernel_phys()` asks `vmm_init()`, which gets the image's physical base by walking the
bootloader's own tables. It asks rather than derives: `virt - 0xffffffff80000000` is zero
only if the bootloader happened to load at physical zero, and it does not, because the
firmware memory map has no entry covering physical page zero at all.

**On QEMU the fetch goes through the page tables, so the address must also be mapped.**
The diagnostic was a `#PF` whose `CR2` sat exactly four bytes past the descriptor's base —
an address nothing in the kernel had, four bytes into a page that was mapped nowhere.

Linux never meets this because its per-CPU area — which is where its TSS lives — sits in a
region it maps at its own physical address, which is what `vmm_map_identity()` does here
for one page: in the kernel's tables, and again in every address space created afterwards.

The intermediate tables for that mapping must be **user-accessible while the leaf is not**.
The TSS page holds the kernel's stack pointers and ring 3 must not reach it, and it lands
in PML4 slot 0 — the slot every user mapping below 512 GiB also goes through. With the
intermediates marked supervisor-only, `walk_to_leaf()` refused every user mapping in that
slot and the ELF loader failed with `elf: could not map 0x3ff000`. Hence
`pt_map_supervised_range()`: walkable intermediates, locked leaf.

Each address space gets its own tables for it, walked from its own PML4 rather than
pointing at the kernel's, so a process mapping anything nearby cannot disturb the kernel's
copy and `vmm_address_space_destroy()` frees only what that space owns.

## 2026-10-06: `ltr` caches the TSS, and a second `ltr` is refused

`ltr` reads the descriptor and *caches* the base, limit and attributes inside the
processor; from then on the TSS is fetched from that cache. Rewriting the GDT entry changes
the bytes in the table and nothing else.

So `gdt_init()`'s `ltr` — which runs first in `kmain()`, deliberately, so a fault is
reportable from the first instruction — cached base zero, because at that point
`vmm_kernel_phys()` correctly refused to answer. The correct descriptor installed after
`vmm_init()` was never seen. QEMU's register dump says so plainly: `TR =0028
0000000000000000 0000006b` — base zero, limit correct, and a right-looking limit is the
misleading part.

Two consequences, both confirmed by experiment rather than reasoned about:

- Refreshing the cache means `ltr` again — but **not** `gdt_reload_for()`, which also
  reloads the data segments and so re-reads `%gs`'s base from the table. The per-CPU base
  lives in `MSR_GS_BASE`, so that turns `percpu_self()` into a NULL dereference in the
  first timer tick: a fault that points at the timer and not at the segment reload that
  caused it. `tss_reload_for()` is the `ltr`-only version, and it exists for that reason.
- A second `ltr` on a TSS that is already busy fails with `#GP` reporting the selector
  (error code `0x28`, the TSS selector itself). So `tss_reload_for()` writes the descriptor
  again first — which clears the busy bit that `ltr` set — and *then* loads it. Only one
  identity-mapped region is tracked, and a second different one is refused rather than
  silently displacing the first.

## 2026-10-06: a preempted ring-3 task is saved as the interrupt frame, whole

The scheduler's switch saved six callee-saved registers and a return address. For a task
interrupted in ring 3 it additionally copied the five words `iretq` consumes — RIP, CS,
RFLAGS, RSP, SS — to just above them.

That is a complete ring-3 *frame* and not a complete ring-3 *context*. `%rax`, `%rbx`,
`%rcx`, `%rdx`, `%rsi`, `%rdi`, `%rbp` and `%r8`–`%r12` came back as whatever the
*switching* task left in them. `hello` resumed holding a `page_table_t` pointer in `%r8` and
dereferenced it, which reads as a wild pointer in the program and was entirely the kernel's.

The fix is smaller than the workaround it replaces. `regs_t` — the block `isr_common()`
already hands `isr_dispatch()` — is contiguous and *ends* in the five words `iretq` wants,
so a ring-3 context is saved as that block, by pointer, and restored by `isr_common()`'s own
tail with the dispatcher's return address left off. Nothing is copied and all fifteen
registers come back.

This also removed a condition rather than adding one. The copy version had to write *above*
`%rsp`, into the frame chain of whoever called `schedule()`, which is only safe because
that task will resume by `iretq` and never come back up that stack. Saving a pointer has no
such condition — and the two tails having to stay in step is now the only thing to remember.

`percpu_t.from_user` says a ring-3 context exists; `percpu_t.user_frame` says where. Both
are recorded by `isr_dispatch()` and both consumed by `schedule()`. The marker is cleared on
the syscall path too, because that path never reaches the scheduler — the door holds IF
clear for its whole length, so a tick cannot arrive inside it and there is nothing to
preempt, which would otherwise leave the marker set for the *next* save to misread.

## A blocking syscall resumes in the kernel, not in ring 3

**Date: 2026-10-07**

A ring-3 task is saved whole -- the interrupt frame the CPU pushed -- and resumed through
`iretq`, which is the only way back to ring 3 with the right CS, SS and %rsp. That is right
for a task that was interrupted and is waiting its turn. It is wrong for a task blocked in
the middle of a syscall: that frame's RIP is the instruction after `int $0x80`, so resuming
it delivers the syscall's return value straight into user code, and the value is whatever is
in `%rax` when the task stopped -- the call *number*, because nothing has computed a result
yet.

So a task that is stopping in the kernel clears the per-processor ring-3 marker before
switching away and is saved the ordinary way, with its C frames intact on its own kernel
stack. Woken, it unwinds back through the syscall, computes the answer, writes it into the
interrupt frame, and that frame's own `iretq` returns to ring 3 properly.

The marker is cleared on the way *in* as well as on the way out, because a blocked task can
come back on a processor that never saw it block, and that processor's marker is whatever
its last ring-3 interrupt left there. Clearing it on the way *out* instead -- which is where
it is *read* for the outgoing save -- wipes it for every ring-3 task, and the self tests
stop dead just after `phase6-elf` with the machine idle and no panic. Both orders are
commented where they are, because the difference between them is invisible until it is
fatal.

The alternative was a restartable syscall: on resume, re-execute `int $0x80` and let the
handler run again. That is what Linux does, and it is the better shape for a syscall that is
not idempotent. It is also a bigger change than this phase needs, and every call here is
either idempotent or safe to re-run.

## A wait queue's `head` is not a cursor

**Date: 2026-10-07**

`wait_queue_t` has a `head` and a `tail`, and `tail` advances as slots are handed out. `head`
is set to zero by `wait_queue_init()` and **nothing ever advances it**, because a slot is not
retired in the order it was queued -- it is retired by whichever task gets woken reclaiming
it, and that can be any of them. The live waiters are therefore scattered anywhere in the
array, and the gap a retired slot leaves is counted as though it were still occupied.

Scanning `[head, head + count)` reads the wrong slots as soon as one waiter in the middle
goes away. The visible result was a wake that found nobody while somebody was demonstrably
waiting: a pipe whose reader had given up its slot, and whose writer was blocked on a full
pipe, both sat there for ever -- nothing faulted, nothing panicked, and it looked like a
scheduler that had stopped making progress.

Both wake functions now scan the whole array. Sixty-four words, on a path taken once per
state change rather than once per byte, so correctness is free here, and a cursor that would
have to be maintained correctly under concurrent retirement is not.

## A pipe's lock is its wait queue's lock

**Date: 2026-10-07**

`wait_queue_sleep()` enqueues and blocks as one step, so a caller that decides to wait based
on state held under a *different* lock has a window in which a waker changes that state
before the slot exists -- and the caller then sleeps for ever on a condition that is already
satisfied. `wait_queue_enqueue_locked()` and `wait_queue_block_locked()` split the two so the
caller holds the lock that guards the condition for the whole of it.

A pipe's buffer is guarded by its wait queue's own lock, which is why that is what it uses,
and why there is one queue for both ends with every state change waking all of them: each
waiter re-checks its own condition after waking, so a spurious wake costs a loop iteration
and only a missed one is a bug -- and waking everybody cannot miss anybody.

A pipe is freed with its last end, and read ends and write ends are counted separately. A
`fork` counts too: the child holds a descriptor for the same pipe, so closing its inherited
copy of the read end -- which every program does, or it could never see end of file -- must
not make the parent's next write fail with `EPIPE` against a reader that is right there.

## `user/hello/hello.h` is kept, and nothing includes it

**Date: 2026-10-12**

An earlier commit deleted `user/hello/hello.h` when the user-visible ABI moved out of it and
into `user/common/minx_user.h`, shared by every program. That is a removal of committed code,
which AGENTS.md forbids, so the file is back -- verbatim, with a banner saying it is
deprecated, and with its include guard renamed so that it cannot collide with the real
header if anything ever pulled in both.

It is restored rather than deleted because the rule exists for a reason, and the reason is
visible in this file's own contents: that header's call numbers were a hand-written copy of
the kernel's enum, and the copy is exactly what went stale when the enum was renumbered and
`user/common/start.S` kept the old `SYS_EXIT`. The interesting part was never the working
code, which was replaced properly; it was the failure, and it is worth being able to read.

Nothing builds against it. The numbers in it are years out of date and a program that
included it would get an exit syscall that is now `lseek`.

## The kernel's own console REPL is deprecated, not deleted

`kernel/drivers/conio.c` implements a command language on the console before `/init` has
finished mounting anything: `help`, `clear`, `uptime`, `meminfo`, `cpus`, `sched`, `ps`,
`keys`, `irqs`, `reboot`, and a prompt that says `type help and press enter`.

It is off by default now, behind `make KERNEL_SHELL=1`, and it is not removed.

The reason is that it and `sh` are two readers of one terminal competing for one keyboard,
and the kernel's copy wins the race because it is already sitting in its loop when `/init`
runs. Anything typed meant for the shell is taken by the kernel before the shell is started,
which is a worse failure than having no prompt at all: the machine looks like it swallowed
your input.

It is kept because it is the right tool for one job — the userspace shell cannot start, and
`ps` and `irqs` and `meminfo` from ring 0 are exactly what you want then — and because the
rule against removing committed code exists precisely so that a decision like this is made
by turning something off and leaving it legible, rather than by deleting the evidence.

What replaces it: nothing. `sh` is the shell, and when it works the kernel has no business
reading the keyboard.

## A bare assignment in `sh` no longer runs a second shell

**Date: 2026-10-14**

`expand_command()` used to fork and `execve("/bin/sh", ...)` with no arguments for a
command whose words were all assignments, on the reasoning that such a command "still runs
something". That is now: set the variables, succeed, and run the redirections in a child if
there are any.

Two things are wrong with the old behaviour and the first is visible in any run — `sh
script.sh` reached the first bare assignment, started an interactive shell whose standard
input was the console, and the script, its parent and the program's `waitpid` all waited
for ever on a shell that was waiting for a person to type at it. The second is that it was
never what anyone wanted even when it did not hang: `PATH=/bin ls` is a command with
*words*, so it takes the ordinary path and sets `PATH` for that one command, with no second
shell anywhere.

This is a behaviour change to committed code rather than an addition beside it, which is the
one case where the no-deletion rule needs a reason beyond tidiness. The reason is that the
old behaviour is a bug with two independent proofs against it and no behaviour anyone could
have wanted, and leaving it selectable behind a flag would mean shipping a shell in which
`x=1` starts an interactive session.

## The ramfs, `kmalloc` and the pmm are locked rather than left to the caller

**Date: 2026-10-14**

All three had **no lock at all** while every processor used them, and each is a
read-modify-write of one shared word: a bitmap bit and a cursor, a free-list head, a
node-pool free-list head.

What made them visible is not that they failed in an obvious way. It is that they did not
fail in one way: a filesystem that ran out of nodes with the pool nowhere near full, a test
that failed once in three, and a machine that stopped with no fault and no message. Each
was recorded on this page as a separate mystery, and the reason they are one bug is that
they are all the same bug.

The alternative considered and rejected is a *per-cpu or per-task* allocator — no lock, but
correct — because it changes the shape of `kmalloc`'s contract everywhere and because the
heap is deliberately one region that the memory report quotes as a single number. The
alternative that is still wanted, and is recorded as a known bug, is a page reference
count: `vmm_address_space_destroy()` frees tables and nothing else, so every frame a
process ever mapped stays out of the pool for the life of the machine. That is correct
today and is the thing that will decide how long this kernel can run.

## `memcpy` refuses an absurd length instead of attempting it

**Date: 2026-10-14**

Sixteen gibibytes is a subtraction that underflowed, and copying it is not slow — it is
fatal, because the walk goes straight through the page tables, and when it reaches them the
machine stops with no message, no fault and nothing on the serial port, because the page
table it was writing through is the one the console would have been printed from.

The alternative considered and rejected is letting it happen and hoping the guard is never
hit, on the grounds that a check in the hottest function in the kernel is a cost. It is two
instructions on a path that is almost never taken, and the cost of *not* having it was three
days of work on a bug whose symptom was silence. A check that turns an unnameable failure
into a named one is the cheapest diagnostic in the tree.

The limit is 16 MiB, which nothing in this kernel copies anywhere near: the biggest copy in
the tree is a 4 KiB page for a copy-on-write fault.

## A hung machine prints its task list

**Date: 2026-10-14**

`sched_tick()` asks whether anything has been printed for ten seconds and, if not, prints
every task. The alternative — leaving hangs silent and reading them out of the guest with a
debugger — was what this project did for the whole of phase 9 and it does not work: a
gdb-attached guest under TCG never reaches the test that is hanging, so every conclusion
drawn from one has been wrong.

Ten seconds is chosen against the *test run*, not against the machine: a run has stretches
where nothing is printed while a program thinks, and a report that fires on those is a
report nobody reads. The boot processor only, because four processors printing four copies
of the same list is one copy of the list and three copies of noise.

## `CR4.WP` is off, and the kernel prepares user pages itself instead

**Date: 2026-10-14**

`CR4.WP` is the right answer to every ring-0 write into a user page: with it set, a
supervisor store to a read-only *user* page takes the same `#PF` a program's store does,
and the page-fault handler already knows how to answer that by giving the page a private
copy. It covers user pages only, so the kernel's own read-only text and data and the
write-only framebuffer mapping are untouched.

**It cannot be set on this machine.** QEMU raises a general protection fault for
`mov %cr4` with bit 16, so `vmm_init()` would panic before the first line of boot output.
Measured on QEMU 11.1.2 with both `-accel kvm` and `-accel tcg,thread=multi`. The bit is
defined in `kernel/include/pagetables.h` with that measurement next to it, so the
experiment is not repeated.

Without it, a ring-0 store to a page a `fork` gave up goes straight through into the
shared physical frame and updates it for both processes — and the syscall return is exactly
that store, because it writes the result into the program's own frame on the program's own
stack. The alternative considered and rejected is leaving it to each call site to remember:
there are three such writes today, a fourth is a matter of time, and the failure mode of
the one that is forgotten is two processes corrupting each other silently. `syscall_prepare_write()`
is called before all three instead, and the page-fault handler now answers supervisor
write faults as well, so the two mechanisms agree.

The cost is a page walk per syscall return, which returns immediately for any task that
has never forked. If a later QEMU models the bit, setting it *and* keeping the three calls
is strictly better than either alone.

## Both allocators carry a redzone, and say so when it is written

**Date: 2026-10-14**

The libc's allocator poisons a freed block and gives every block a 16-byte redzone past
the end of its payload; the kernel's `kmalloc()` does the same. A write past the end of a
block lands in the next block's header or the next allocation's payload, and what it
looks like from the outside is memory corruption with no witness.

The argument for it is one afternoon. `cmd_free()` in the shell was releasing two pointers
twice each, and *nothing* in the tree pointed at it: the allocator spun, the kernel's
`memcpy` guard fired for a different bug, the copy-on-write list was blamed and rebuilt,
the page tables were blamed and duplicated. The redzone reported the function, the caller
and the offset on its first run.

The argument against it is that it is a check in the hottest allocator in the system. It
is sixteen bytes of memset and a sixteen-byte compare, against an allocator that already
memsets every payload on the way out. A check that turns an unnameable failure into a
named one is the cheapest diagnostic there is, and this project has now paid twice for not
having one.

Reported, not refused: a refusal would turn a use-after-free into a failed `malloc()`, which
is a different failure with a different cause and would hide the one that is there.

## Two double frees were left in place deliberately, and documented

**Date: 2026-10-14**

`cmd_free()` and `expand_assign()` each released memory they had already given away —
`c->stages` and `c->arms` twice in the first, the string it had just handed its caller in
the second. Both are fixed in place, which the no-deletion rule allows ("fixing bugs by
editing lines in place"), and both are ordinary bugs with no behaviour anyone could have
wanted: the observable effects were a shell that spun in its allocator and a script whose
first assignment hung the machine.

What is *not* obvious, and is recorded here because the rule's default reading is more
cautious than this case needs: a duplicated `free()` of the same pointer twice in one
function is not a feature that a flag can select between. Keeping both behind an option
would mean shipping a shell whose `x=1` can either work or corrupt the heap, which is a
worse artefact than either version alone.

## `fork` copies every user page, because `CR4.WP` cannot be had

**Decision.** `vmm_fork_aspace()` gives the child a private frame for every user page at the
moment of the `fork`. It does not mark pages copy-on-write and hope for a write fault.
`fork_copy_page()` in `kernel/mm/vmm_user.c` is the implementation.

**Why.** `CR4.WP` is the bit that makes a read-only page mean something. It is not set,
because QEMU 11.1.2 treats bit 16 of CR4 as reserved and raises #GP for `mov %cr4` with it
set — measured three ways (`-accel kvm`, `-accel tcg,thread=multi`, `-cpu max`) and on the
register form rather than the immediate one, so it is QEMU's CR4 mask and not the encoding.
With the bit clear, a store to a read-only *user* page does not trap: it writes the frame.

Every piece of the copy-on-write design assumed the opposite. The observable cost was a
child that resumed perfectly — `fork` returned zero, the frame said `rax = 0` and the right
`rip`, the child branched on `pid == 0` — and then its `ret` popped a stack word the parent
had already reused for a later call's return address, so it landed past the branch and ran
the parent's code. The parent's `trace` call, twenty instructions later on a different
processor, was enough. That is the whole of the intermittent "fork returns non-zero in the
child", and it was reported as a scheduler bug for days because nothing anywhere said a
page was shared.

**Alternatives considered.**

- *Set `CR4.WP` anyway.* Not available; see above.
- *Eager copy of writable pages only, share read-only ones.* Cheaper, and it leaves the
  same class of bug one level down: a program that writes its own text would corrupt its
  twin, silently, which is the identical failure with a rarer trigger.
- *Keep marking, and have `syscall_prepare_write()` cover the parent's writes as well.* It
  already does, and it was never the problem: `syscall_prepare_write()` covers the three
  *kernel* stores into user memory. The parent's own ring-3 stores are nothing's business
  but the processor's, and the processor is not watching.
- *One `PTE_COW` bit read at fault time is cheaper.* There is no fault to read it at.

**Cost.** Memory proportional to the parent's user address space per live child, instead of
proportional to what it writes. Bounded by `COW_MAX_PAGES` (1024 pages, 4 MiB) per `fork`;
past that `fork` fails and says so. A shell forks once per command substitution and forks
about eight times in `shtest`, so the shell's whole address space is copied eight times and
freed as the children exit.

**Left in place deliberately.** `PTE_COW`, `vmm_cow_fault()`, the `cow_list_t` record and
`syscall_prepare_write()` all still exist and are still correct; a `fork` simply no longer
produces the state they exist to resolve. They are also the only thing that has to change
the day a target with a working `CR4.WP` arrives — the marking is one function, and it is
written so that putting it back is putting it back.

**Why not record this as "a workaround".** It is not a degraded copy-on-write standing in
for the real thing. On a machine where a write to a read-only page does not trap, copying
at the `fork` *is* the correct implementation, and the lazy version is the incorrect one:
it is only correct because of a processor feature the kernel cannot ask for. The cost is
real and is stated above; the correctness is not a compromise.

## `exec`'s scratch is per processor, not per machine

**Decision.** The arena `sys_exec()` copies argument and environment strings into is one per
CPU (`exec_arenas[MAX_CPU_COUNT]`), not one `static` for the kernel.

**Why.** It was one static array, justified as "not reentrant, and does not need to be:
`exec` replaces the program, so there is no second `exec` in flight from the same task". The
premise is true and the conclusion does not follow, because the invariant that would make it
safe is *per task* and the shared state is *per machine*. A pipeline has two tasks calling
`exec` at the same instant on two processors. They wrote at the same `used` offset and each
took the other's pointers, so `ls /tmp/shtest-* | head -n 1` handed `ls` the arguments of
`head -n 1` about one run in four.

Nothing faulted. The pointers were valid — for somebody else — which is why the only symptom
was a program rejecting an option it had never been given, in a script that otherwise ran to
completion.

**Alternatives considered.**

- *A lock around the arena.* Rejected for two reasons. The arena has to stay valid for the
  whole of `elf_load()`, which maps segments and builds a stack, so the lock would have to
  be held across page allocation and into the loader's own locking; and the contention is
  real, not theoretical, since the only thing that makes two `exec`s overlap is exactly the
  pipeline that the shell runs hundreds of times.
- *Put the arena in `task_t`.* Correct, and about 8 KiB per task on a 16 KiB stack for
  `task_t` alone. Per-CPU is free and equally correct.

**Why per-CPU is safe without a lock.** A task is on exactly one processor at a time, and
`syscall_dispatch()` holds interrupts off for the whole of a handler, so a CPU never has two
`exec`s in flight. That is the same argument that makes the static arena safe per task, moved
up one level to where the sharing actually was.

## Every program depends on the library it links, marker or not

**Decision.** `$(LIBC)` is a prerequisite of every user program's rule in the Makefile.
The `libc` marker still chooses the entry point and nothing else.

**Why.** `-lc` is on every one of those link lines, so every program is linked against the
libc whether or not it says so — and the marker says which *entry point* a program gets
(`main()` through `libc/src/exit.S`, or `minx_main()` through `user/common/start.S`), not
whether its calls are answered by the library that happens to be on disk now.

The consequence was that a fix to anything in `libc.a` reached only the marked programs.
`qsort()` was repaired — it merged a five-element array's last pass two-and-three instead of
four-and-one — and `sort` and `ls`, which call it, carried on running the broken copy,
because nothing in the build said they had been built against the library they were linked
with. Nothing complained. The programs simply ran yesterday's library.

**This is the kind of bug that costs a day and is found in a minute by a tool that did not
exist yet.** `coretest` found it the first time it ran a `sort` after the `qsort` fix and
found it *unchanged*.

**Left as it is.** A program with no marker still gets `minx_main()` and `start.S`. That is
the right default for a test of the syscall door or the loader, which wants nothing in
between it and the kernel — `forktest`, `cowtest` and `sigtest` all rely on it. The
dependency is now unconditional; the entry point is still per program.

## A libc function with no test is a libc function that is broken

**Decision.** `qsort()` gets a test in `libctest` covering every length from 1 to 20, three
input shapes per length, stability, and the empty and single-element cases.

**Why it was written the way it is.** The bug was in the *merge*, and it only appeared from
the third pass of a bottom-up merge sort, where the two runs being merged are of different
lengths. The lengths that matter are exactly the ones that are not powers of two — 3, 5, 6,
7, 9, 11, 13, 15, 17, 19 — so a test of sizes 1, 2 and 4 passes straight through it. That
is why this is a loop over every length rather than three cases.

It had no test at all before, which is the only reason the bug survived: `glob()`, `ls` and
`sort` all use it, and all three were visibly wrong in ways nobody traced back to it.
