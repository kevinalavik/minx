# STATUS

Current phase: 5 (scheduler) — phases 1 to 4 VERIFIED; the scheduler works and its
six tests pass, but an intermittent multi-processor fault keeps the phase from
being marked VERIFIED. See "Known bugs / gaps" for exactly what is and is not
proven, and start there.

## Phases
| # | Phase | State | Evidence / command |
|---|-------|-------|--------------------|
| 1 | Toolchain & boot, framebuffer console | VERIFIED | `make test` → 17/17 PASS, framebuffer dump captured; `make run` boots to a graphical console at 1280x800 with 160x50 text cells. Serial log shows the memory map, RSDP, `SMP: 4 CPUs`, initrd and heap. Panic path verified separately (register dump + stack trace on both consoles) while fixing bring-up bugs. |
| 2 | CPU core, keyboard into console | VERIFIED | `phase2-acpi` (4 processors with distinct LAPIC ids, IOAPIC at 0xfec00000), `phase2-interrupts` (IOAPIC owns all 16 lines, 8259s stay masked, a mask round trip changes the chip's own bit, and both the LAPIC timer and the 8253 really fire over 120 ms with interrupts on), `phase2-keyboard` (8042 self test, scancode set 1, reachable on IRQ 1), `phase2-input` (event ring in and out in order). Typed keys reach the graphical console: `tools/type-test.sh` types `help`, `keys`, `irqs`, `uptime`, `meminfo` and all five answer. |
| 3 | Memory | VERIFIED | `phase3-pmm` (frames page-aligned, mapped, distinct, hold their contents, free count survives an alloc/free round trip), `phase3-pmm-contiguous`, `phase3-direct-map` (bottom/middle/top of RAM translate to themselves; one frame past the last usable frame is not mapped), `phase3-pmm-churn` (4096 allocations through 64 slots, no frame handed out twice). `meminfo` reports 68 MiB heap with 87 KiB used. |
| 4 | SMP | VERIFIED | `make test` → 28/28 (34/34 with phase 5's tests added). All 4 processors come up through the bootloader's hand-off and stay up. `phase4-percpu` (`%gs` resolves to the calling processor's own slot; every known processor has a distinct slot and LAPIC id), `phase4-atomics` (32000 increments from 4 processors at once, no update lost), `phase4-locks` (32000 contended ticket-lock critical sections across 4 processors, exact), `phase4-lockfair` (every processor served every round), `phase4-mcs`, `phase4-mutex`, `phase4-semaphore`, `phase4-irqsave`, `phase4-ipi` (reschedule, wake and TLB-shootdown IPIs delivered and acted on), `phase4-tss`, `phase4-smp`. Verified on 1, 2, 3, 4 and 8 processors. `cpus` reports per-processor interrupt, IPI and idle counters. || 5 | Scheduler & kernel threads | IN PROGRESS | **Implemented; passes, but intermittently. Over fourteen `-smp 4` runs of the test ISO: eleven 34/34 and three failures.** `make test` needs 34/34, so the phase is not VERIFIED. The fault is localised to a corrupted per-processor `current` pointer — see "Known bugs / gaps".  `kernel/include/task.h`, `kernel/lib/task.c`, `kernel/arch/x86_64/switch.S`. Preemptive per-CPU run queues, an idle task per processor, work stealing, virtual-runtime fairness, timed sleep, tagged wait queues. `phase5-queues` (every processor takes ticks; a processor with nothing to switch to makes no switches and says so), `phase5-parallel` (6 tasks all complete; on more than one processor they demonstrably ran on more than one, on one processor they are shown to have been interleaved), `phase5-preempt` (a task that never yields is switched out and back in at least once), `phase5-fair` (two equal-weight tasks get the same order of magnitude), `phase5-sleep` (a 60 ms sleep measures 60 ms), `phase5-waitq` (waking one tag wakes exactly that waiter). A good `-smp 4` run reads `sched: cpu 0: 41439 ticks, 11149 switches, 5558 steals` and `sched: 6 tasks ran on 4 processor(s)`. **NOT VERIFIED: it is intermittent.** Roughly one run in three at `-smp 4` dies instead — see "Known bugs / gaps". |
| 6 | Userspace entry | NOT STARTED | |
| 7 | Processes, signals, pipes | NOT STARTED | |
| 8 | VFS + ramfs + initrd + devfs | NOT STARTED | |
| 9 | TTY + libc + sh | NOT STARTED | |
| 10 | Coreutils + procfs | NOT STARTED | |
| 11 | Hardening / stress | NOT STARTED | |
| 12 | Polish | NOT STARTED | |

## What it took to bring the application processors up
The blocker, and what actually unblocked it.

**The approach was wrong, not the machine.** Everything was written to do INIT/SIPI
by hand: a trampoline taking an application processor from real mode to the kernel,
copied to a page below 1 MiB, and two startup IPIs. It never worked, and the
measurements said why:

- A startup IPI alone does not wake these processors. They sit where the bootloader
  left them and the SIPI is ignored, because they have already consumed the
  startup-interrupt handshake.
- An INIT wakes them, and it kills the machine. INIT is a level interrupt whose
  destination field is ignored, so it resets the sender as well as the targets —
  hence the "all excluding self" shorthand, which is the one encoding it honours.
  A released processor then fetches from the reset vector at `0xfffffff0`, where
  hardware holds the firmware's wait-for-startup loop and this machine holds the
  *bootloader's* code. The released processor executes it, faults with no IDT of
  its own, and triple-faults. With `-no-reboot` that ends the run, which is why the
  machine died a few lines after "AP trampoline" rather than reporting a failed
  processor. The kernel cannot redirect that address from its own page tables: a
  released processor is in real mode with paging off, so the reset vector is a
  physical address and page tables are not consulted at all.

**Limine has already started every application processor.** Reading its
`common/smp.c` settled it: for each application processor but the boot one, the
bootloader runs its own INIT/SIPI and then parks the processor in a loop spinning
on one field of its per-processor structure — `mp_info->goto_address` — jumping to
it the moment it becomes non-zero. The kernel's job is to write that field, not to
send interrupts at all. `smp_start_all()` now does that, which is why the hand-off
also explains a second thing: the parked processors are running on the *bootloader's*
page tables, so the bootloader's memory must not go back to the allocator until the
hand-off has finished. That is what `vmm_release_boot_tables()` is for, and it is
now called after the hand-off rather than at the CR3 switch.

The INIT/SIPI path is kept (`smp_start_cpu()` and `smp_trampoline.S`) because it
is the only implementation that does not need the bootloader's cooperation. It is
not called automatically, and the comment at the sequence says why.

Four faults had to be found and fixed to make the hand-off work, all of them
silent:

1. **`vmm_walk_translate()` returned a page frame instead of an address.** A leaf
   entry's address field is the start of whatever it maps, so the offset inside the
   mapping was being dropped — and the offset had already been masked off at the top
   of the walk, which is why adding it back did not help at first. Every address in
   a page translated to that page's base: right for one address in 4096. The
   `phase3-direct-map` probe only used page-aligned addresses, so it could never
   see it; it now probes unaligned ones too.
2. **`mp_info->reserved` is the stack, not scratch.** The bootloader's trampoline
   switches the processor onto it before returning, so writing the kernel's own use
   of that word into it sent the processor onto a stack the kernel has no idea
   about. `extra_argument` is the only genuinely free field.
3. **`gdt_reload()` never loaded the table.** It reloaded the segments and the task
   register but not the `lgdt`, so a processor arriving from the bootloader's
   hand-off used the *bootloader's* descriptors. Its far return to selector 0x08
   found a 32-bit code segment, the processor dropped into compatibility mode, and
   the next fetch faulted.
4. **One TSS descriptor cannot be shared.** Loading a task register sets the
   descriptor's busy bit *in the table*, so the boot processor's `ltr` left the
   entry marked and every other processor's `ltr` was refused with #GP and the
   selector as the error code. Each processor now has its own TSS, its own
   descriptor slot, and its own IST stacks. The `phase4-tss` self-test checks that.

Two more faults turned up on the way, both independent of SMP:

- **`ltr` with a 32-bit operand.** Written as `ltr %ax` it assembles to the 32-bit
  form, which is undefined and raises #GP; the operand-size prefix has to be written
  out. The boot processor got away with it because the compiler folded the constant
  there.
- **The TSS field offsets were all four bytes low.** RSP0 was at 0x04, where a
  32-bit TSS keeps SS:0, and IST 1 at 0x24 rather than 0x28. The CPU reads the real
  offsets, so a ring transition would have loaded RSP0 as zero and a fault would
  have found no IST stack — neither of which anything in phases 1 to 3 exercised.

## What was blocking phases 2 and 3, and what fixed it
Every one of these was silent: no warning, no fault, and in three cases the code
looked like it was working.

1. **LINT0 in ExtINT delivery.** Limine leaves the local APIC's LINT0 entry in
   ExtINT mode and masked. ExtINT makes the APIC take the vector from its own LVT
   entry and ignore the one the IOAPIC put in the message, so every device line
   would arrive on the same vector. LINT0 is now unmasked in fixed delivery.
2. **The LVT Timer mask bit was never cleared.** A masked LVT counts down and
   never interrupts, which is indistinguishable from a timer whose interrupts are
   being dropped somewhere else.
3. **IOAPIC register indices were wrong twice.** The redirection table starts at
   window offset 0x10 and each entry is two registers wide, so entry *n*'s low
   half is at `0x10 + 2n`; the routing code addressed `2n`, which reads back as
   zero and looks exactly like a chip that ignores every mask change. Separately,
   the "level" flag was written to bit 8, which is the delivery-mode field — the
   trigger mode is bit 15 — so every line was programmed edge-triggered with
   lowest-priority delivery.
4. **`ioapic_route()` conflated the pin with the vector.** It added its second
   argument to the GSI base as though it were a pin number, but the caller passes
   a vector, so every entry was written past the end of the redirection table.
5. **The legacy ISA lines were level-triggered.** As level-triggered entries the
   chip latches the interrupt in its remote-IRR bit and re-asserts it for as
   long as the line is high; on the PS/2 controller that means a keystroke
   handled forever. They are edge-triggered now.
6. **The ISA IRQ to IOAPIC pin map cannot be assumed.** The architecture
   suggests identity (pin *n* aliases ISA IRQ *n*) and real hardware does that,
   but QEMU's IOAPIC model routes the 8253's IRQ0 to **pin 2**. A kernel that
   programs the identity map sees a 100 Hz clock counted as IRQ2 and never sees
   a timer tick — the chip delivers a vector and nothing else, so a wrong pin is
   indistinguishable from a dead line. `timer.c` now uses the 8253, the one
   legacy device whose line the kernel can raise on demand, to find its own pin
   with every other pin masked; a line displaced by that probe reports itself
   unroutable rather than being silently accepted.
7. **PML4[511] cannot be used for a self-map.** The direct map and the kernel
   image share that PML4 entry, so writing a pointer to the PML4 there turns
   every kernel address into a walk that ends "not present" and the machine
   triple-faults the instant CR3 is loaded. Reachability of every page-table
   frame through the direct map is proved instead.
8. **`pmm_reserve_phys_range()` rounded inwards**, handing out the page the
   kernel image's own tail lives in.
9. **The bootloader's reclaimable memory is live until CR3 is replaced.**
   Limine's page tables are still in use, so those ranges are held across the
   switch and released afterwards.
10. **The kernel image's physical address is not `virt - 0xffffffff80000000`.**
    It is found by walking the bootloader's own tables.
11. **`pt_get()` stopped one level short**, handing back the parent entry.
12. **`pmm_free_phys_page()` and `pmm_free_phys_range()` tested for the frame
    being *free* before counting it**, so a real free was never counted and a
    double free was. Memory was never returned to the pool.
13. **`vmm_translate()` did not add the in-page offset** for a large-page leaf, so
    every address in the first half of a 2 MiB page translated to the page's own
    base.
14. **The TSC frequency was guessed, not measured.** All three CPUID leaves that
    can report it returned zero on this machine, and the 1 GHz fallback was
    wrong in the direction that makes delays *short*: a 120 ms window was 37 ms.
    The TSC is now measured across the 8253's channel-2 gate window
    (65536/1193182 s of exactly known time) — 3194 MHz here. `monotonic_ns()` also
    divided to whole seconds before scaling, so `monotonic_ms()` could only ever
    read a multiple of 1000.

## What exists today
- **Boot**: Limine 12.9.1, hybrid BIOS+UEFI ISO, higher-half kernel at
  `0xffffffff80000000`. Memory map, framebuffer, RSDP, MP info and the initrd
  module are all taken from the bootloader's responses, plus an HHDM request so
  the direct map offset comes from the bootloader rather than a constant.
- **Console**: own framebuffer renderer (`kernel/drivers/fb.c`) plus an
  ANSI/VT100 terminal (`kernel/drivers/console.c`) with SGR colours, 24-bit
  colour, bold/inverse attributes, scrolling regions, cursor movement and
  erase. Everything is mirrored byte-for-byte to COM1.
- **Font**: the 8x16 `lat9-16.psf` from the Linux `kbd` package, converted to a
  C array at build time by `tools/psf2c.py`. No font scaling: a 1280x800
  framebuffer yields a 160x50 terminal.
- **CPU core**: GDT (ring 0/3 + 64-bit TSS), TSS with IST stacks for #DF and
  NMI, IDT with all 32 exceptions plus every vector through 0x3f (the 16 legacy
  IRQ lines, the LAPIC timer, the APIC error vector, the spurious vector and the
  IPI vectors), and an exception reporter that decodes page-fault error codes and
  dumps registers and a stack trace to both consoles.
- **ACPI**: RSDP, RSDT or XSDT, MADT (including x2APIC and LAPIC-override
  entries), FADT discovery. `minx.verbose=1` dumps raw table bytes.
- **Interrupt controllers**: 8259 remap and full masking, LAPIC (xAPIC and
  x2APIC) and IOAPIC enumeration and programming over uncacheable windows, the
  legacy lines routed to `0x20..0x2f` edge-triggered, a runtime probe for the
  8253's pin, per-line handlers, counters and names. The 8259s stay masked for
  good once the IOAPIC owns delivery.
- **Memory**: bitmap physical allocator over the firmware's usable ranges with
  page-granular alloc/free, outward-rounded range reserve and release, and
  held/released boot ranges; four-level page tables with 4 KiB, 2 MiB and 1 GiB
  mappings; a kernel mapping, an HHDM direct map of all of physical memory,
  uncacheable device windows and a write-combining framebuffer window, switched
  in with a single CR3 load.
- **Time**: the TSC measured against the 8253, the LAPIC timer calibrated
  against the same window, the 8253 as a 100 Hz IRQ0 time base, and an RTC
  reader for the wall clock.
- **Input**: a lock-free SPSC input queue, a PS/2 keyboard driver that
  negotiates scancode set 1 (or asks the 8042 to translate set 2), decodes
  shift/ctrl/alt/meta/caps/numlock and pushes key events, and a console
  command loop that echoes them and answers `help`, `meminfo`, `irqs`, `keys`,
  `cpus`, `uptime`, `clear` and `reboot`.
- **Panic**: prints the message, a full GPR dump and a kernel stack trace, then
  writes 1 to QEMU's isa-debug-exit port so the harness sees a crash.
- **Allocator**: first-fit free list with coalescing over regions handed to it
  explicitly. A 4 MiB static arena is attached at boot and 64 MiB more comes
  from the physical allocator once the direct map is live.
- **SMP**: all four processors online, each with its own stack, IST stacks, TSS
  and GDT descriptor, local APIC timer and IDT/GDT/TSS reload. Started through the
  bootloader's own per-processor hand-off (`mp_info->goto_address`) — see above.
  IPIs are sent through the local APIC's ICR with a delivery wait, with handlers on
  all four IPI vectors, and a per-CPU work slot that a reschedule IPI drains; that
  is the seed of the run queues. The INIT/SIPI path is kept but not used.
- **Locking**: `kernel/include/spinlock.h` and `kernel/lib/spinlock.c` — atomics
  (add/sub/CAS on a `lock`-prefixed instruction), ticket locks, MCS locks, a mutex,
  a semaphore, the interrupt-save spinlock variants, per-CPU lock flags, and the
  three barriers. Order and rules are in `docs/LOCKING.md`.
- **Scheduler**: `kernel/include/task.h`, `kernel/lib/task.c`,
  `kernel/arch/x86_64/switch.S`. One run queue and one idle task per processor;
  `schedule()` takes a queue lock, picks, drops the lock and only then switches, so
  no lock is ever held across a context switch and there is no lock ordering to get
  wrong. Fairness is by virtual runtime scaled by weight. Context switching is six
  pushes, a store and six pops.
- **Console commands**: `sched` (per-processor ticks, switches, steals, migrations,
  queue depth, idle wakeups) and `ps` (every task, its state, weight, last processor,
  switch count and milliseconds run).
- **Self tests**: thirty-four in-kernel checks (`minx.test=1`) plus a
  host-compiled allocator fuzz test (`make test-host`).

## Known bugs / gaps
- The mutex and the semaphore spin instead of sleeping. Correct, and right for a
  short critical section; wrong for one that can block, which is why the wait
  queues they would need are not written yet either. Phase 5.
- `kprintf` has no lock, so several processors announcing themselves at once
  interleave their output on the framebuffer and the serial mirror ("smp: 2x" etc).
  Spinlocks are phase 5; until then the interleaving is cosmetic and the `cpus`
  command reports the real per-CPU state.
- **THE BLOCKER FOR PHASE 5. A corrupted per-processor `current`, intermittently,
  at 4 or more processors.** Measured over eight `-smp 4` runs of the test ISO:
  five passed 34/34, one hung, one failed a check, and one panicked. `-smp 1` and
  `-smp 2` have not been seen to fail. It happens early, usually within
  `phase1-console-scrolling`.
  What is known, and each of these was established by experiment rather than by
  argument. **Both symptoms have been reduced to one cause, and it is now a guard
  that names it rather than a fault that does not:**
  `smp: task 1 (boot) is already linked into the blocked list and is being linked
  into that same list again (state 4)`.
  `state 4` is `TASK_SLEEPING`, and the call chain is
  `test_run_all` → `task_sleep_ms` → the list check. So `task_sleep_ms()` is being
  called on a task that is *already asleep and already on the blocked list*, which
  happens only if the `schedule()` inside it **returned without switching away**.
  `schedule()` has exactly four ways to return without switching, and three of them
  are correct; this is the remaining question:
  1. `!sched_up` — ruled out, the tests run.
  2. `c->in_schedule` — the guard is set and cleared inside `schedule()` with
     interrupts off, so nothing should find it set.
  3. `next == prev` — correct, and only reachable by the idle task re-picking itself.
  4. **the `TASK_RUNNABLE → TASK_RUNNING` compare-exchange on the chosen task
     fails**, and `schedule()` returns. This is the one to instrument next: log
     `next`, its `state`, and `next->id` on that path and see what state it was in.
     A plausible route is a task stolen by `steal_any()` that another processor
     claims first — the steal removed it from the victim's queue, and if the
     compare-exchange then fails the task is on nobody's queue and the processor that
     stole it stays on its own task.
  The other symptom, `sched: processor 2 has 0xffff800000100030 as its current task`,
  is a heap payload being used as a `task_t *`, and is very likely the *same* bug
  arriving from the other side: a walk that left a list, or a task lost by (4), leaves
  a processor's `current` pointing at whatever the memory had become.

  Also ruled out, each by experiment:
  - **A stack overflow.** Task and application-processor stacks were raised from
    16 KiB to 32 KiB and the fault did not change. A guard word at the bottom of every
    task stack plus a per-tick `%rsp` check (`sched_check_stack()`) now make an
    overflow report itself rather than corrupt a return address; it has never fired.
  - **`isr_common` losing `%rbp`.** It does not lose it: `%rbp` is callee-saved and
    survives the handler chain even when a handler switches stacks. Changing it to
    save the frame address on the stack instead turned the intermittent fault into a
    deterministic hang at exactly the same place, which is how the theory was
    disproved. Reverted.
  - **`irq_restore()`'s `popfq` restoring stray trap or direction flags.** Replacing
    the whole-word `popfq` with `sti`/`cli` on the interrupt-enable bit alone hung the
    same guest at exactly the same place, every time. Reverted.
  Next things to try, in order:
  1. Instrument the failed compare-exchange in `schedule()` as above.
  2. `-d int,cpu_reset -D build/q.log` on a failing run, and check whether the
     compare-exchange fails in a steal or after a pop from the processor's own queue.
  3. If it is a steal, make `steal_from()` re-queue the task on the *victim* when the
     claim fails rather than dropping it.

- The framebuffer resolution is whatever Limine picks (1280x800 under QEMU
  here); the `resolution:` option in `boot/limine.conf` is not honoured by the
  BIOS path, so the console adapts to whatever arrives.
- The initrd is unpacked by nothing yet: it is loaded and its size is reported,
  but phase 8 writes the ramfs unpacker.
- On QEMU, IRQ2 has no redirection entry of its own because the 8253's pin took
  it; the line stays masked and claiming it fails. That is correct once the
  8259s are masked for good, but it would need revisiting if a real IRQ2 device
  ever appears.
- `PTE_CACHE_*` still encodes the PAT field in bits 6:7 rather than the PAT MSR
  or the PWT/PCD/PS combination the SDM describes, so "uncacheable" for MMIO is
  currently write-combining. Nothing depends on the distinction yet.
- The serial mirror emits raw ANSI escape bytes for colours and cursor moves.
  That is honest (a dumb serial terminal renders them as characters) but it puts
  control bytes at the start of some self-test result lines, so the harness
  cross-checks the guest's own tally rather than trusting the line count alone.
- `make run` opens a GUI window; automated verification uses `make test`, which
  is always headless.
- **Fixed, but worth remembering:** `make -j8 iso iso-test` used to stage both ISOs
  through one shared directory, each rule `rm -rf`-ing the directory the other was
  halfway through filling. The test ISO then came out carrying the *run* ISO's
  `limine.conf`, so `minx.test=1` was never passed, the guest booted to the
  interactive console instead of running the tests, and `make test` sat there until
  the harness reported "the guest never reached TEST-READY". It looks exactly like a
  kernel that ignores its command line. Each ISO now stages into its own directory;
  verified by building both with `-j8` and booting the test ISO repeatedly.

## Needs human decision
(none yet)

## Test commands
```
make test        # headless boot, in-kernel self tests, framebuffer dump; non-zero on failure
make test-host   # host-compiled allocator fuzz test (fast, no QEMU)
make run         # graphical console in a QEMU window (SMP=4 MEM=512M by default)
GUI=0 make run   # headless: serial console only
make debug       # boot paused for GDB (tools/gdb-run.sh drives it in batch)
sh tools/shot.sh build/minx.iso /tmp/shot.ppm 10   # screenshot the framebuffer
sh tools/type-test.sh build/minx.iso /tmp/t.log 'help
keys'                                                 # type at the console headlessly
```

## Push status
`git push origin HEAD` fails in this environment with
`could not read Username for 'https://github.com'` — no credential helper and no
token available, and AGENTS.md forbids putting tokens in files or remotes.
Everything is committed locally in logical commits with the model co-author
trailer; push as soon as credentials exist. The earlier phase-one attempt hit
the same wall (see commit "docs: record phase one push limitation"), and every
phase since has committed locally for the same reason.
