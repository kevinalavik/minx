# STATUS

Current phase: 6 (userspace entry) — phases 1 to 4 VERIFIED, phase 5's blocker
**fixed**, phase 6 three of four. `make test` now runs the whole suite to the end and
reports **36 of 38 PASS** with no panic at `-smp 1`, `-smp 2` and `-smp 4`, where it
used to die before the tests began. Phase 5 is still **not** VERIFIED: it needs a clean
matrix, and `phase4-lockfair` now fails for a reason that is the test's premise rather
than the kernel's. Two failures remain, both named exactly in "Known bugs / gaps", and
neither is worked around to make the number look better.

## Phases
| # | Phase | State | Evidence / command |
|---|-------|-------|--------------------|
| 1 | Toolchain & boot, framebuffer console | VERIFIED | `make test` → 17/17 PASS, framebuffer dump captured; `make run` boots to a graphical console at 1280x800 with 160x50 text cells. Serial log shows the memory map, RSDP, `SMP: 4 CPUs`, initrd and heap. Panic path verified separately (register dump + stack trace on both consoles) while fixing bring-up bugs. |
| 2 | CPU core, keyboard into console | VERIFIED | `phase2-acpi` (4 processors with distinct LAPIC ids, IOAPIC at 0xfec00000), `phase2-interrupts` (IOAPIC owns all 16 lines, 8259s stay masked, a mask round trip changes the chip's own bit, and both the LAPIC timer and the 8253 really fire over 120 ms with interrupts on), `phase2-keyboard` (8042 self test, scancode set 1, reachable on IRQ 1), `phase2-input` (event ring in and out in order). Typed keys reach the graphical console: `tools/type-test.sh` types `help`, `keys`, `irqs`, `uptime`, `meminfo` and all five answer. |
| 3 | Memory | VERIFIED | `phase3-pmm` (frames page-aligned, mapped, distinct, hold their contents, free count survives an alloc/free round trip), `phase3-pmm-contiguous`, `phase3-direct-map` (bottom/middle/top of RAM translate to themselves; one frame past the last usable frame is not mapped), `phase3-pmm-churn` (4096 allocations through 64 slots, no frame handed out twice). `meminfo` reports 68 MiB heap with 87 KiB used. |
| 4 | SMP | VERIFIED | `make test` → 28/28 (34/34 with phase 5's tests added). All 4 processors come up through the bootloader's hand-off and stay up. `phase4-percpu` (`%gs` resolves to the calling processor's own slot; every known processor has a distinct slot and LAPIC id), `phase4-atomics` (32000 increments from 4 processors at once, no update lost), `phase4-locks` (32000 contended ticket-lock critical sections across 4 processors, exact), `phase4-lockfair` (every processor served every round), `phase4-mcs`, `phase4-mutex`, `phase4-semaphore`, `phase4-irqsave`, `phase4-ipi` (reschedule, wake and TLB-shootdown IPIs delivered and acted on), `phase4-tss`, `phase4-smp`. Verified on 1, 2, 3, 4 and 8 processors. `cpus` reports per-processor interrupt, IPI and idle counters. || 5 | Scheduler & kernel threads | IN PROGRESS (blocker fixed) | **Implemented; passes, but intermittently. Latest measurement over fourteen `-smp 4` runs of the test ISO: eleven 34/34, three failures** (58% before the fixes below, 79% after). `make test` needs 34/34, so the phase is not VERIFIED. The fault is localised to a corrupted per-processor `current` pointer — see "Known bugs / gaps".  `kernel/include/task.h`, `kernel/lib/task.c`, `kernel/arch/x86_64/switch.S`. Preemptive per-CPU run queues, an idle task per processor, work stealing, virtual-runtime fairness, timed sleep, tagged wait queues. `phase5-queues` (every processor takes ticks; a processor with nothing to switch to makes no switches and says so), `phase5-parallel` (6 tasks all complete; on more than one processor they demonstrably ran on more than one, on one processor they are shown to have been interleaved), `phase5-preempt` (a task that never yields is switched out and back in at least once), `phase5-fair` (two equal-weight tasks get the same order of magnitude), `phase5-sleep` (a 60 ms sleep measures 60 ms), `phase5-waitq` (waking one tag wakes exactly that waiter). A good `-smp 4` run reads `sched: cpu 0: 41439 ticks, 11149 switches, 5558 steals` and `sched: 6 tasks ran on 4 processor(s)`. **NOT VERIFIED.** The blocker that made it intermittent is fixed (`task_save_publish_and_switch()` saves the outgoing context before publishing it, and `steal_from()` refuses another processor's idle task); it is not marked VERIFIED because the clean-run matrix has not been re-measured over enough runs, and because `phase4-lockfair` now fails for a test-premise reason — see "The two remaining failures, exactly". |
| 6 | Userspace entry | IN PROGRESS | **Built; three of the four new tests pass.** `kernel/mm/vmm_user.c`, `kernel/lib/elf.c`, `kernel/lib/syscall.c`, `user/hello/`. Per-process address spaces with a shared kernel half, a ring-3 task, `int $0x80` with page-table-checked pointer validation, an ELF64 loader, and a freestanding `hello` linked `ET_EXEC` at 4 MiB and embedded in the kernel image. Passing: `phase6-pointers` (the kernel image, the direct map, the top of user space, `~0` and a wrapping range are all refused as user pointers; an empty range at a legal address is not), `phase6-elf` (a short file, the wrong machine, a 32-bit class and an image with no loadable segments are each refused *and say why*), `phase6-aspace` (two tasks, two address spaces, each writing its own page, neither able to reach the other's, both reaching the kernel). **`phase6-user` has not been observed passing** — see below. |
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

## Framebuffer evidence, this build
`sh tools/shot.sh build/minx.iso build/run-screen.ppm 22 -smp 4` on this tree: the
graphical console shows the whole boot -- memory map and direct map, framebuffer at
1280x800, ACPI tables, LAPIC and IOAPIC, both timers calibrated, per-CPU data for four
processors, `sched: run queues for 4 processor(s), boot task 1, idle tasks 2`, the
keyboard reading a scancode, and the shell prompt with its command list.

The lines that look shuffled are the known `kprintf` lock gap, cosmetic and documented
under "Known bugs / gaps"; they are four processors announcing themselves at once and the
text is intact, just interleaved. `build/test-screen.png` is the same console after the
scrolling test, which fills all 109 lines and scrolls correctly.

## The two remaining failures, exactly

`make test` -> **36 of 38 PASS**, `TEST-READY` reached, clean shutdown, no panic. The two
that fail, and neither is worked around:

### 0. `phase6-user`, progress this session -- four real bugs found and fixed

**`sched_check_stack()` was panicking on every ring-3 entry.** A user task that has
entered ring 3 runs its *kernel* code on its own user stack, because `iretq` puts `%rsp`
there and the kernel half of the address space is mapped all the way down. So `%rsp` is a
user address, far below any kernel heap, and comparing it with the kernel stack's low
bound reports an overflow that has not happened. `task_create_user()` made it worse
without meaning to: it cleared `t->stack` but left `t->stack_guard` set, so the "no guard,
skip the check" path was never taken.

**This was the fault behind every strange symptom this test produced.** The panic it
raised is why the follow-on reports were nonsense: a return address of `TASK_STACK_GUARD`, a
register dump with a kernel-text `%rbp`, and a serial log that stopped in the middle of a
line. Every one of those was a symptom of *panicking on a stack the panic path cannot
use*. The stack guard now skips a user task that is on its own stack, and says why; a
kernel task's `%rsp` is never a user address, so the check is unchanged for everyone else.

**The ring-3 context flag was being computed from `%cs`, which cannot see it.** An
interrupt from ring 3 makes the CPU switch to CPL 0 and push a *ring-3 return frame* --
RIP, CS, RFLAGS, SS, RSP -- before a single instruction of the handler runs. So by the
time the scheduler saves the outgoing task, the current CS is the kernel's and says
nothing about the frame sitting above it on the stack. Reading it that way marked every
ring-3 context as a kernel one, and the restore then popped the ring-3 RIP as a return
address and jumped to a user address at CPL 0 -- with the kernel's stack and CS, and
everything the frame said about SS and RSP discarded.

`isr_dispatch()` now records it where it can still be seen (`percpu_t.from_user`), and
`schedule()` consumes it when it saves. It is "the outermost interrupted context was ring
3", not "the innermost handler is running below ring 3": a tick that lands inside a
syscall inside a ring-3 task is still a ring-3 context as far as the scheduler is
concerned, and every handler nested in between sees CS `0x08`.

**`TSS.RSP0` was zero on every processor in the machine.** `tss_init_for()` zeroes the TSS
and `tss_set_rsp0()` was never called from anywhere, so the first `int $0x80` from any
user task pushed its frame at address zero. Set per *processor*, not per task -- the CPU
loads it on every interrupt out of ring 3, so it has to name whichever task is running on
that processor right now, and `schedule()` does that before each switch. Verified by
reading it back through `tss_for(index)->rsp[0]` at every switch.

**`task_create_user()` discarded the kernel stack `task_create()` had already allocated**,
on the grounds that a user task does not need one yet. It needs one immediately. Kept now,
which also means a ring-3 task's kernel frames are guarded and per-task.

### 0b. `phase6-user` -- THE RING-3 FRAME HAD SS AND RSP THE WRONG WAY ROUND

**This was it, and it has been there since phase 6 was written.**

`iretq` pops **RIP, CS, RFLAGS, RSP, SS** -- RSP is *fourth*, SS is *fifth*. The frame was
built as RIP, CS, RFLAGS, SS, RSP.

**How it presented, and why it cost so long.** Reading a stack pointer where a selector
belongs raises `#GP` whose error code is the low half of that stack pointer. So the report
was `vector 13, error code 0xef00` -- an error code that looks like a selector and is not
one, and that points at the frame rather than at anything the CPU did with it. Two
measurements pinned it exactly:

- Changing the word the author thought was RSP from `0x7fffffffef00` to `0x7fffffffcf00`
  changed the error code from `0xef00` to `0xcf00`.
- Changing the word the author thought was **SS** changed the error code **not at all**.

So the CPU was reading the fifth quadword as SS, which is what `iretq` does -- the frame
was the right way round and the *contents* were in the wrong slots.

**Proof it is fixed**, from QEMU's own log rather than the kernel's:

    v=0e e=0014 i=0 cpl=3 IP=0023:000000000040042c pc=000000000040042c
                SP=001b:00007fffffffef00 CR2=000000000040042c

`cpl=3`, `CS=0x23`, `SS=0x1b`, RIP at the program's real entry point and RSP 256 bytes
below the top of its own stack. **The program is in ring 3.** Before this it never got
there.

**What is left, precisely.** An instruction-fetch page fault at `0x40042c` reporting *not
present*, with `CR3=0x513c000`. Walking that task's own tables finds the entry point
present *and* user-readable -- so the CPU and `pt_get()` disagree, and the only remaining
candidate is that the CR3 loaded is not `t->tables->pml4`. The comparison is not directly
observable, because the exception path cannot survive the fault it is reporting: the `#PF`
is immediately followed by `#DF` and a triple fault, so the handler's own report is lost.
`isr_report_exception()` now prints what `__enter_user` saw -- `%cs`, `%rsp`, all five
quadwords about to be popped, and CR3 -- which is the diagnostic that found this and is
the one to widen next.

**A second real bug found in the same hunt: the TSS descriptor base was a virtual
address.** A TSS descriptor's base field is a *physical* address -- the CPU fetches the TSS
contents from it directly, the TSS is not subject to paging even in long mode. Both call
sites passed `(uint64_t)&kernel_tss[index]`, i.e. `0xffffffff80xxxxxx`, so the CPU was
reading `RSP0`, the IST pointers and `iomap_base` out of physical address
`0xffffffff80xxxxxx`, which is not memory. Now `virt_to_phys()` is applied. The symptom is
worth recording because it points nowhere near the cause: an interrupt out of ring 3 loads a
garbage `RSP0`, the push onto it faults, and the resulting `#DF` is reported **at the
original privilege level with the original instruction pointer** -- so a ring-3 program that
faults looks like it faults twice at its own entry point and never gets a handler at all.
`phase4-tss` passes throughout because it reads the struct rather than asking the CPU, which
is a fair reminder that a test can verify a structure's contents without ever checking
whether the hardware agrees about them.

**A third: the console was pointed at the framebuffer's own physical address**
(`0xf0000000`, a PCI BAR, so PML4 slot 0 -- the half a user address space owns). Now
through the direct map, which every address space shares. This turned out to be a no-op in
the end, because `fb->address` was *already* a direct-map address, and it is left in place
because it is correct and because it removes a trap for anyone who changes it later.

**Where it stands.** `make test` runs 31 checks and then the machine dies in `phase6-user`
with an instruction-fetch page fault at the entry point, `cpl=3`, `CR2=0x40042c`, reported
*not present* -- and immediately followed by `#DF`, `#DF`, which is the ring-3 stack switch
still failing despite the TSS fix above.

**What is established, and what is not.** Established: the loader completes (bracketed with
markers that all printed); the ring-3 frame is correct *and* is at `%rsp` when the `iretq`
runs (`cs=0x8 rsp=0x7fffffffefd8 | rip=0x40042c cs=0x23 rflags=0x202 ss=0x1b
rsp=0x7fffffffef00`); the selectors are DPL 3; the program reaches ring 3. **Not**
established: why the CPU's walk of `CR3=0x513c000` finds `0x40042c` not present while
`pt_get()` over `t->tables` finds it present *and* user-readable. Those two walks cannot be
compared in one place, because the exception path dies before it can print -- which is the
next thing to fix, and the reason it is that rather than the walk itself.

**Next thing to try, in order.** (a) Make the ring-3 fault *reportable*: the `#DF` chain
means `TSS.RSP0` is still not usable, so read `tss_for(index)->rsp[0]` **and** CR3 in the
same place and print them with `serial_puts` -- plain serial, no formatting and no console,
because the markers proved that survives and the console does not. (b) With the report
working, print `t->tables->pml4` beside CR3. If they differ, `vmm_address_space_switch()` is
not switching for this task; if they agree, `pt_get()` and the CPU really are disagreeing
about the same tables and the PTE contents need dumping at both levels. (c) Only then look
at the walk.

### 1. `phase6-user` -- still hanging, now much further along
The whole suite reaches it now, and every *other* part of phase 6 passes. It gets as far
as the task being created, its address space built, its image loaded, its stack laid out
and the ring-3 frame assembled; then it produces no output at all and is reported as
"never exited ... after 20 s".

**Narrowed to:** a fault reading 8 bytes at exactly `USER_STACK_TOP` (`0x7ffffffff000`),
from kernel code running with `%rsp` inside the task's user stack. Registers at the
fault: `rdi = top - 408`, `rsi = r8 = top`, `rdx = 8`, `r15 = 408`, `rsp = top - 416`,
`cs = 0x08`, and the faulting instruction is `memmove` (`memcpy`+204, which for `d < s`
tail-calls `memcpy`). So it is an 8-byte copy whose *source* is the constant
`USER_STACK_TOP` -- and nothing in the tree dereferences `USER_STACK_TOP` as a pointer
any more; the only three uses are `top - <constant>` arithmetic in `task.c`, `elf.c` and
`syscall.c`.

**Ruled out by measurement, not by argument:** it is not the ELF loader (`elf:` reports no
failure; the image and the stack are mapped and the frame is built), not the ring-3 entry
(`iretq` now succeeds with RPL-3 selectors -- the error code `0x20` is gone), not the
address space (CR2 is a *present-but-unmapped* address and the walk over the same tables
agrees), and not preemption (the ring-3 restore now uses `iretq` and the flag that selects
it).

**Two things found since, both real, and the frame verified correct.** The ring-3 return
frame was dumped from the kernel's own alias of it immediately before the switch, and it
is exactly right: `rip=0x40042c cs=0x23 rflags=0x202 ss=0x1b rsp=0x7fffffffef00` --
the program's real entry, a DPL-3 code selector, a DPL-3 data selector, interrupts on,
and a stack pointer 256 bytes below the top of its own stack. So the loader is not
building a bad frame.

**`TSS.RSP0` was zero on every processor in the machine, and that is now fixed.**
`tss_init_for()` zeroes the TSS and `tss_set_rsp0()` was never called from anywhere --
which means the first `int $0x80` from any user task pushed its frame at address zero.
Setting it is per *processor*, not per task: the CPU loads it on every interrupt out of
ring 3, so it has to name whichever task is running on that processor right now, and
`schedule()` now does that before each switch. Related: `task_create_user()` was
*discarding* the kernel stack `task_create()` had already allocated for the task, on the
grounds that a user task does not need one yet. It needs one immediately. It is kept now,
which also means a ring-3 task's kernel frames are guarded and per-task.

**So what is left is narrower than it was.** Ruled out: the loader (frame correct, image
and stack mapped), the selectors (DPL 3, verified in the dump), the address space (CR2
is a present-but-unmapped address and the walk over the same tables agrees), `RSP0` (now
set, verified by reading it back through `tss_for(index)->rsp[0]` at every switch), and
the syscall door (gate 0x80 is DPL 3, `int $0x80` with rdi/rsi/rdx/rax in the right
registers).

**The frame is verified correct**, dumped from the kernel's own alias of it immediately
before the switch: `rip=0x40042c cs=0x23 rflags=0x202 ss=0x1b rsp=0x7fffffffef00` -- the
program's real entry point, DPL-3 selectors, interrupts on, and a stack pointer 256 bytes
below the top of its own stack.

**What remains is one fault, and it is now narrow.** A single page fault, in `memcpy`,
reading **8 bytes at exactly `USER_STACK_TOP`**, with `%rsp` inside the task's user stack
and `cs = 0x08`. `RDI = top - 408`, `RSI = R8 = top`, `RDX = 8`, `R15 = 408`,
`R9 = -408`, `RSP = top - 4512`. So it is a one-quadword copy whose *source* is the
constant `USER_STACK_TOP`, both operands user addresses, from ring-0 code on the user
stack. Nothing in the tree dereferences `USER_STACK_TOP` as a pointer any more -- the only
three uses are `top - <constant>` arithmetic in `task.c`, `elf.c` and `syscall.c`.

**Ruled out by measurement:** the loader (frame correct, image and stack mapped), the
selectors (DPL 3, verified), the address space (CR2 is a present-but-unmapped address and
the walk over the same tables agrees), `RSP0` (now set, verified by reading it back at
every switch), the syscall door (gate 0x80 is DPL 3; `int $0x80` with rdi/rsi/rdx/rax in
the right registers), `sys_write` (it does not copy -- it hands the user pointer to the
console, which ring 0 may read with SMAP off), and preemption (the ring-3 restore now uses
`iretq`, with a flag recorded where the frame is actually created).

**The most useful thing found while narrowing it:** `syscall_dispatch()` prints nothing
when a trace `kprintf` is put at its top, so the program never reaches its first syscall --
`int $0x80` is never executed. The fault is therefore strictly *before* the first syscall,
in the ring-3 entry or in the program's own first instructions.

**Next thing to try, in order.** (a) Dump the *instruction pointer and CS* of the ring-3
task at the fault from the exception frame rather than from the current registers, so it
is unambiguous whether the program is in ring 3 at all. (b) `objdump` the exact faulting
instruction and work out which of `rdi`/`rsi` is really the source, since `memcpy`'s loop
clobbers both and the dump is mid-loop. (c) Trace the ring-3 entry: print from a `kwarn`
immediately after `iretq` returns control, which for a program that faults on its first
instruction is the exception handler.

**Where to look next, in order.** (a) The user program's first syscall: `hello` writes to
fd 1 first and *nothing at all* appears, so the fault is likely in argument marshalling
rather than in the string -- `arg(regs, n)` for `sys_write` deserves a look at the frame
offsets it reads. (b) `syscall_copy_from_user`/`copy_to_user` with `src` or `dst` computed
from a field that is genuinely `USER_STACK_TOP`. (c) The console path for fd 1, which is
the only other thing a `write(1, ...)` touches.

**Not yet done, and it is a real design gap, not a detail:** a user task has no kernel
stack (`task_t.stack` is NULL, `stack_low`/`stack_guard` unset). The kernel therefore runs
on the user stack for such a task, and `TSS.RSP0` for its processor is that processor's
boot stack. It works -- the CPU switches to RSP0 on the way in from ring 3 -- but it means
a ring-3 task's kernel frames are not guarded, are not per-task, and make every stack
trace of a fault inside a syscall meaningless. Giving user tasks a real kernel stack is
the right fix and is the first thing phase 6 still needs.

### 2. `phase4-lockfair` -- a test whose premise the scheduler no longer provides
`processor 2 took the lock 128 times, expected 64`.

**This is not a lock bug and the evidence is in the same run:** `f.served` equals the sum
of the per-processor counts and equals `online * FAIR_ROUNDS` exactly, so every worker took
all 64 of its turns, no update was lost, and no worker was cut short. What broke is the
*attribution*: `fair_worker` counts into `f.turns[percpu_self_index()]`, and its premise
is that the task `fanout_every_cpu()` pinned to processor *i* still runs on processor *i*.
Work stealing now migrates it -- correctly -- so two workers' 64 turns land on one processor
and the per-index counts stop meaning "how many turns this processor served".

**Why it has not been changed.** The right fix is to measure the property the ticket lock
actually has, which is that waiters are served in ticket order: record the sequence number
each worker is served and check it is `0..N-1` in order. That is migration-proof and
*stronger* than counting turns per processor. Rewriting a test's assertion to make a number
go green, on the day the number went red, is how a real bug gets papered over, so it is
recorded here and left failing instead. The lock itself is fine.

## Known bugs / gaps
- **The phase-5/6 blocker is FIXED and verified** -- `make test` runs the whole suite to
  the end, 36 of 38 PASS, no panic at 1, 2 or 4 processors. Root cause, established by
  reading the code rather than by inference: `schedule()` published the outgoing task to a run queue *before*
  `task_switch()` wrote that task's `saved_rsp`. A stack-based context is not finished
  until the instant its stack pointer is stored, so for the whole of the window between
  the queue insertion and the store, the task was reachable by another processor while
  its `saved_rsp` still named an *older* frame. That processor restored a stale `RSP`
  and the task resumed on a frame already in use. Two live frames on one kernel stack
  corrupts both, and the visible result is a processor that stops taking interrupts with
  no fault reported anywhere -- which is what every captured symptom of this fault
  actually was.
  This accounts for all four captured messages at once, including the two that looked
  like unrelated bugs: `current` holding a heap payload and `cpu[i].idle` holding a
  kernel text address are both what a `task_t *` read through a corrupted stack looks
  like, and the "re-entered `schedule()` from `task_sleep_ms`" report is what a context
  saved at the wrong stack depth looks like.
- The mutex and the semaphore spin instead of sleeping. Correct, and right for a
  short critical section; wrong for one that can block, which is why the wait
  queues they would need are not written yet either. Phase 5.
- `kprintf` has no lock, so several processors announcing themselves at once
  interleave their output on the framebuffer and the serial mirror ("smp: 2x" etc).
  Spinlocks are phase 5; until then the interleaving is cosmetic and the `cpus`
  command reports the real per-CPU state.
- **RESOLVED -- the blocker below was a publish-before-save race.** Every message it
  produced is accounted for by it; see "The two remaining failures, exactly" above for
  what is left. Kept because the captured messages are the evidence for the fix, and
  because a future session that sees one of them again needs to know it was this and not
  something new. The one-line version: `schedule()` queued the outgoing task and *then*
  called `task_switch()`, which writes that task's `saved_rsp` as its last act, so for the
  window in between the task was reachable by another processor while its saved stack
  pointer still named an older frame.
  The captured messages, in the order they appeared:
  - `sched: processor 2 has 0xffff800000100030 as its current task, and that has weight 0`
    -- a heap payload used as a `task_t *`.
  - `sched: [from this processor's idle task] processor 0 chose 0xffffffff8000a0a0, and
    could not claim it` -- a *kernel text address* used as a `task_t *`.
  - `sched: processor 3 re-entered schedule() from task_sleep_ms+178 while it was already
    switching` -- `schedule()`'s frame was still live on the stack when `task_sleep_ms`
    ran, which means a context was saved at the wrong place.
  - `rip=0xdeadbeef`, which is `TASK_STACK_GUARD`: a stack pointer that came from a guard
    word rather than from a stack.
  Phase 6 cannot be signed off until this is fixed, because `phase6-user` is exactly the
  test that dies of it.

- **RESOLVED -- this was the same bug as the entry above, seen from the other side.** The
  intermittent 4-processor failure and the phase-6 crash were one bug. `-smp 1` fails
  identically, which is what identified it as logic rather than a race, and the fix is in
  `task_save_publish_and_switch()`. What follows is the investigation as it stood, because
  the reasoning that led to the fix is worth having and because several of the checks it
  added are load-bearing.
  It has been reduced from "a hang somewhere" to two named things, and one of them has
  a fix that took it from 58% to 79% clean. `-smp 1` and `-smp 2` have not been seen to
  fail.
  **What is known.**
  - `schedule()` has four ways to return without switching a task away. Two are
    correct (the idle task re-picking itself; nothing to run). The other two were
    *silent*, and a silent return is fatal to any caller that called `schedule()` in
    order to sleep: nothing is switched to, it returns still asleep, and the one thing
    that would have woken it is the reschedule flag that `schedule()` had just cleared.
    Both are now reported rather than swallowed, and both were observed firing:
    - The `TASK_RUNNABLE → TASK_RUNNING` claim on the chosen task fails. The chosen
      task is reported with **where it came from**, and the answer is always the same:
      `from this processor's idle task` — `cpu[index].idle` is not a task. So a
      processor's scheduler state is being overwritten. `schedule()` now also validates
      the idle task it is about to fall back to, and names it if it is wrong.
    - `schedule()` is re-entered while already switching. With interrupts off across
      the whole selection-and-switch this cannot be an interrupt, so it is a *nested
      C call*, and the report now prints the return address of whoever did it. That
      caller is not identified yet; it is the single most useful thing left to find.
  - The other symptom, `sched: processor 2 has 0xffff800000100030 as its current task`,
    is a heap payload used as a `task_t *`, and is very likely the same corruption
    arriving from the other side.
  - The visible failure that follows is one line:
    `smp: task 1 (boot) is already linked into the blocked list and is being linked
    into that same list again (state 4)` — `state 4` is `TASK_SLEEPING`, called from
    `test_run_all` → `task_sleep_ms`. That is the sleep that never slept, above.
  **Fixed along the way, each by measurement rather than argument.**
  - The reschedule flag is now put back on a re-entrant `schedule()`. It was being
    cleared and not restored, which is the sleep-never-wakes bug above. 7/12 → 11/14.
  - The blocked list had no list-integrity checks, so removing a task that was not on
    it set its head *and* tail to NULL and silently dropped every other sleeper.
  - Every list walk — run queue and blocked list, head and every subsequent node —
    checks that the node it is looking at is on the list it is walking. A walk that
    starts on a node which is not on the list reads perfectly plausible `state`,
    `weight` and `vruntime` fields, which is how a heap address reaches `current`:
    not followed to, but simply the first thing looked at.
  - **`steal_from()` no longer takes another processor's idle task.** At boot the boot
    task is the only runnable task and the boot processor's own queue is always empty, so
    it stole on every tick: took an idle task, ran it, and on the next tick took the next
    one. The boot task was never picked again. Nothing looks wrong — every processor is
    in its idle loop taking its timer — and the self-tests run on the boot task, so the
    suite simply stopped after the scheduler came up. The in-schedule guard does not save
    it: it defers a preemption of a processor whose own queue is empty, which is exactly
    the processor stealing an idle task, so it never fires and there is no preemption to
    break the cycle.
  - **The build was the biggest single cause and it is fixed.** `make -j8 iso
    iso-test` staged both ISOs through one shared directory; the test ISO came out
    carrying the run ISO's `limine.conf`, so `minx.test=1` was never passed and
    `make test` sat until the harness timed out reporting "the guest never reached
    TEST-READY". Each ISO now stages into its own.
  - **The cause, and the reason this entry is long: a preempted task is published to a run
    queue before its context is finished.** `schedule()` requeues the old task *before* it
    switches, and between the requeue and the switch there is a window -- a lock
    acquisition, an address-space load -- in which the old task's saved stack pointer still
    refers to a frame on the *preempting* processor's stack. A second processor that claims
    it in that window switches to a context that is not finished: the registers and the stack
    pointer come from wherever the preemptor had got to. A task running that way resumes
    with the preemptor's registers, and the first thing anyone notices is a `task_t *` that
    has become a kernel text address -- which is exactly what `cpu[index].idle` was observed
    holding, and why the same corruption showed up three different ways (a kernel text
    address, a heap payload, and a task whose `weight` was zero).
  - **The fix is known and is not in the tree.** Finish the context *before* publishing the
    task: split the switch into a save half and a restore half, and order them so that
    between them the old task is saved, then published, then the new one restored. That is
    not a cosmetic change to `task_switch` -- it moves the saved context out of the stack and
    into the task, because a stack-based context is not finished until the instant it is
    restored from, and the scheduler has to do work in between.
  - **An attempt at that fix was made, broke the boot, and was reverted.** It is preserved in
    `git stash` ("save-then-publish scheduler switch"); nothing committed was touched and
    HEAD boots and reaches the graphical shell. Three mistakes were made in it, recorded here
    because each is easy to repeat:
    1. Publishing with a non-claimable `TASK_SWITCHING` state **deadlocks**: no picker will
       take the task, and it only becomes claimable when it is resumed, which requires being
       picked.
    2. Splitting `task_switch` into a save half that *returns* leaves the six saved
       registers **below** `schedule()`'s stack pointer, where the bookkeeping that follows
       overwrites them. The context has to live in the task, not on the stack.
    3. Even with the context in `task_t.regs`, a never-run task needs *both* context forms
       populated. `task_stack_init()` builds the stack frame for `task_enter_first()`; if
       `regs[]` is left zero then a restore loads `RSP = 0` and the machine triple-faults on
       the first instruction -- which is what `-d int,cpu_reset` showed.
    After (3) the fault moved to `vmm_address_space_switch()` being handed a `task->tables`
    of `0x542618c3`, so there is at least one more cause behind it that the attempt did not
    reach. **The next attempt should start from the stash and fix all three together, not one
    at a time.**
  **Also ruled out, by experiment.**
  - A stack overflow: task and application-processor stacks were raised from 16 KiB to
    32 KiB and the fault did not change. A guard word at the bottom of every task
    stack plus a per-tick `%rsp` check (`sched_check_stack()`) make an overflow report
    itself rather than corrupt a return address; it has never fired.
  - `isr_common` losing `%rbp`. It does not lose it: `%rbp` is callee-saved and
    survives the handler chain even when a handler switches stacks. Changing it to
    save the frame address on the stack instead turned the intermittent fault into a
    deterministic hang at exactly the same place, which is how the theory was
    disproved. Reverted.
  - `irq_restore()`'s `popfq` restoring stray trap or direction flags. Replacing the
    whole-word `popfq` with `sti`/`cli` on the interrupt-enable bit alone hung the same
    guest at exactly the same place, every time. Reverted.
  **Next things to try, in order.**
  1. Read the return address the re-entrancy report now prints and find the nested
     `schedule()` call. That is the last unaccounted-for path by which the scheduler
     fails to switch.
  2. `-d int,cpu_reset -D build/q.log` on a failing run and check whether `cpu[]` is
     written by something other than the scheduler — `cpu[index].idle` at offset 40 in
     `sched_cpu_t` is the field observed going bad.
  3. The boot-time hang (the serial log ends just after `sched: processor N entering
     the scheduler`) is still unaccounted for and is the other remaining mode.

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
