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
