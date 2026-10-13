# STATUS

Current phase: 9 (libc and shell) — **phases 1 to 8 VERIFIED; phase 9's libc VERIFIED,
phase 9's shell IN PROGRESS.**

`make test` reports **`run-tests: PASS`, 44 of 44 checks**, the guest shuts down cleanly,
`TEST-READY` is reached, the tally is readable and the framebuffer dump is captured.
`libctest` reports **115 of 115** checks passing inside that run.  Verified from a clean
tree (`rm -rf build && make test`) three times consecutively on 2026-10-13, and again after
this session's four kernel fixes (below).

**The suite is flaky and now visibly so.**  Four `make WITH_SHELL=0 test` runs after the
fixes in this session gave 44/44, **43/44** (`libctest: FAILED -- PATH is set before
main()`), 44/44, and two `WITH_SHELL=1` runs gave a `ramfs: no node left` exhaustion
during `libctest` and a kernel panic after `exec`.  The previous note on this page said one
bad run in about six; it is closer to one in three now, and the failing check is not always
the same one.  That is a regression in reliability even where the tally is 44/44, and it is
not yet explained — see "Known bugs".

A normal boot (`make iso`, `-smp 4`) comes up with a graphical console, mounts `/dev` and
`/proc`, finds `/init` in the initrd, runs it, and `init` execs `/sbin/init`, which mounts
`/proc`, puts `/dev/console` on 0/1/2, takes a session and forks `/bin/sh`.  The shell is
not in the default image yet (see "Known bugs"), so the shell's absence is reported on the
console rather than being silent.

### What phase 8 added

The VFS, with a mount table and paths resolved through it rather than by special cases;
`/dev` and `/proc` as filesystems behind the same interface as the root; the file syscalls,
every user pointer bounds-checked; a line discipline; and `/init`.

Three bugs in this phase were only visible because something was looking for them, which
is the whole argument for the new test:

- **`user/common/start.S` had a literal `SYS_EXIT`.** Renumbering the syscall enum in the
  phase-8 work updated `minx_user.h` and left the assembly alone, so `movl $4, %eax` became
  `lseek`. Every program printed its last line and then spun in the entry stub's "exit does
  not return either" loop for ever. Three tests failed with three unrelated-looking
  messages, and the common cause was a number in an assembly file. The numbers are now
  *generated* from the kernel's own enum (`tools/gen-syscall-table.sh`), in two forms --
  a C header and an assembler include -- because an assembly file cannot see a C header's
  macros, and a literal in a second language is a second thing to forget.
- **A registered task was freed.** `program_exec_into()` builds a scratch task to load a
  program into and `kfree`s it, but `task_create_user()` had already put it on the
  all-tasks list. Every walk of that list then followed a dangling entry. It surfaced as a
  page fault in `procfs_status()` reading a task freed by an `exec` several operations
  earlier.
- **`d_type` was `S_IFREG` in a `uint8_t`.** `S_IFMT` is 0170000, so the cast made every
  directory entry say "unknown", and no `.` or `..` was listed at all -- so `find` could not
  terminate and `chdir("..")` resolved to nothing.

The console also got a write lock, which is not a phase-8 change but is what made the rest
of it verifiable: four processors writing into one buffer produced lines like
`iinnitte:r rsutaprttsin egn,apbild e`, and a boot log nobody can grep is not evidence.
Eight consecutive boots now produce zero interleaved lines.

## Phases
| # | Phase | State | Evidence / command |
|---|-------|-------|--------------------|
| 1 | Toolchain & boot, framebuffer console | VERIFIED | `make test` → 17/17 PASS, framebuffer dump captured; `make run` boots to a graphical console at 1280x800 with 160x50 text cells. Serial log shows the memory map, RSDP, `SMP: 4 CPUs`, initrd and heap. Panic path verified separately (register dump + stack trace on both consoles) while fixing bring-up bugs. |
| 2 | CPU core, keyboard into console | VERIFIED | `phase2-acpi` (4 processors with distinct LAPIC ids, IOAPIC at 0xfec00000), `phase2-interrupts` (IOAPIC owns all 16 lines, 8259s stay masked, a mask round trip changes the chip's own bit, and both the LAPIC timer and the 8253 really fire over 120 ms with interrupts on), `phase2-keyboard` (8042 self test, scancode set 1, reachable on IRQ 1), `phase2-input` (event ring in and out in order). Typed keys reach the graphical console: `tools/type-test.sh` types `help`, `keys`, `irqs`, `uptime`, `meminfo` and all five answer. |
| 3 | Memory | VERIFIED | `phase3-pmm` (frames page-aligned, mapped, distinct, hold their contents, free count survives an alloc/free round trip), `phase3-pmm-contiguous`, `phase3-direct-map` (bottom/middle/top of RAM translate to themselves; one frame past the last usable frame is not mapped), `phase3-pmm-churn` (4096 allocations through 64 slots, no frame handed out twice). `meminfo` reports 68 MiB heap with 87 KiB used. |
| 4 | SMP | VERIFIED | `make test` → 28/28 (34/34 with phase 5's tests added). All 4 processors come up through the bootloader's hand-off and stay up. `phase4-percpu` (`%gs` resolves to the calling processor's own slot; every known processor has a distinct slot and LAPIC id), `phase4-atomics` (32000 increments from 4 processors at once, no update lost), `phase4-locks` (32000 contended ticket-lock critical sections across 4 processors, exact), `phase4-lockfair` (every processor served every round), `phase4-mcs`, `phase4-mutex`, `phase4-semaphore`, `phase4-irqsave`, `phase4-ipi` (reschedule, wake and TLB-shootdown IPIs delivered and acted on), `phase4-tss`, `phase4-smp`. Verified on 1, 2, 3, 4 and 8 processors. `cpus` reports per-processor interrupt, IPI and idle counters. || 5 | Scheduler & kernel threads | VERIFIED | Preemptive per-CPU run queues, an idle task per processor, work stealing, virtual-runtime fairness, timed sleep, tagged wait queues. `phase5-queues` (every processor takes ticks; a processor with nothing to switch to makes no switches and says so), `phase5-parallel` (6 tasks all complete; on more than one processor they demonstrably ran on more than one, on one processor they are shown to have been interleaved), `phase5-preempt` (a task that never yields is switched out and back in at least once), `phase5-fair` (two equal-weight tasks get the same order of magnitude), `phase5-sleep` (a 60 ms sleep measures 60 ms), `phase5-waitq` (waking one tag wakes exactly that waiter). The intermittency was the publish-before-save race plus idle-task stealing, both fixed; the clean-run matrix is three consecutive `-smp 4` runs at 38/38 with `TEST-READY`, no panic and no stray bytes in the serial log. A good run reads `sched: cpu 0: 26562 ticks, 16830 switches, 0 steals`. **VERIFIED.** `kernel/include/task.h`, `kernel/lib/task.c`, `kernel/arch/x86_64/switch.S`. |
| 6 | Userspace entry | VERIFIED | Per-process address spaces with a shared kernel half, a ring-3 task, `int $0x80` with page-table-checked pointer validation, an ELF64 loader, and a freestanding `hello` linked `ET_EXEC` at 4 MiB and embedded in the kernel image. `kernel/mm/vmm_user.c`, `kernel/lib/elf.c`, `kernel/lib/syscall.c`, `user/hello/`. `phase6-pointers` (the kernel image, the direct map, the top of user space, `~0` and a wrapping range are all refused as user pointers; an empty range at a legal address is not), `phase6-elf` (a short file, the wrong machine, a 32-bit class and an image with no loadable segments are each refused *and say why*), `phase6-aspace` (two tasks, two address spaces, each writing its own page, neither able to reach the other's, both reaching the kernel), `phase6-user` (the program is loaded, entered at ring 3, reads its own `argc` and `argv[0]`, writes to the console, is refused EBADF for a bad descriptor, EFAULT for an unmapped pointer, EFAULT for a kernel pointer and ENOSYS for an unimplemented call, is preempted in ring 3 and resumed, and exits 0). **VERIFIED** by `make test` → `run-tests: PASS` and by three `-smp 4` runs at 38/38. |
| 7 | Processes, signals, pipes | VERIFIED | `fork` with copy-on-write, `wait`, a per-process descriptor table and pipes, `exec`, and signals. `phase7-fork` (four children, four pipes drained, all reaped by `wait`, no children left), `phase7-cow` (the parent's 8192 bytes survive the child's writes through shared pages), `phase7-signal` (a handler runs with the right number and the interrupted registers are intact; a blocked signal is *pending*, not lost, and arrives on unblocking; a signal with no handler does its default thing, SIGCHLD ignored and SIGUSR2 fatal). See "Phase 7, where it got to". |
| 8 | VFS + ramfs + initrd + devfs | VERIFIED | `phase8-ramfs` (the tree, from inside the kernel) and `phase8-vfs` (`fstest`, the same filesystem through `int $0x80` and nothing else -- create, write, read back through a new descriptor, the mode surviving, the errno for each of six distinct mistakes, a kernel address refused as a path, rename with both halves checked, a failed rename not creating its destination, a hard link being one inode with a count of 2 that drops to 1 when one name goes, a directory refusing to be hard-linked, `lstat` a link and `stat` the file, `readlink` giving a *relative* target, `getdents` listing every entry once each with the right `DT_*`, `rmdir` refusing a non-empty directory, and `/proc/self/status` describing the reading process). `make test` → 43/43, three runs. A normal boot mounts `/dev` and `/proc`, finds `/init` in the initrd and runs it; `init` execs `/bin/hello`, which reports `hello: ok`. |
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

## What finally fixed phase 6

Four bugs, in the order they were found. All four failed *silently*: no assertion fired, no
test went red for the right reason, and every one of them had a symptom that pointed
somewhere else. Two of them are still visible in the write-ups further down, which are left
in place because the trail is the useful part.

### The TSS had one reserved word too many

`tss_t` had `reserved0` *and* `reserved1`, so every field after the top of the structure
sat four bytes above where the processor reads it. RSP0 came out of the reserved word,
which is zero: the first interrupt out of ring 3 pushed its frame onto address zero. IST1
came out of `reserved2`, also zero: the double-fault handler had no stack either, so `#DF`
became a triple fault. And `iomap_base` landed on a reserved word, which still means "no
bitmap", so the test that checks it passed.

The offsets are now `rsp[0]` 0x04, `ist[0]` 0x24, `iomap_base` 0x66. Two independent
authorities agree and they were both worth reading: QEMU's `get_rsp_from_tss()` computes
the stack pointer's offset as `8 * level + 4`, and Linux's `struct x86_hw_tss` is
`{ u32 reserved1; u64 sp0; u64 sp1; u64 sp2; ... }` — the 32-bit TSS's `esp0` slot widened
to 64 bits, which is why the 64-bit TSS keeps it at 0x04 rather than 0x08.

Why phases 1 to 5 passed regardless: an exception at CPL 0 needs no stack switch, so the
CPU pushes onto the stack it is already on. Nothing about the ring-3 boundary works until
something switches stacks, and that is the first thing `phase6-user` does.

**The test that would have caught it reads the struct, not the offsets.** `phase4-tss`
checked `reserved1 == 0 && reserved2 == 0`, which a struct full of zeros satisfies however
it is laid out. It now asserts `offsetof(tss_t, rsp[0]) == 0x04` and friends, because the
processor reads a byte at a fixed offset and not a field by name.

### The TSS descriptor's base is a physical address, and QEMU reads it through the page tables

A TSS descriptor's base is fetched by the CPU by physical address; the TSS is not subject
to paging even in long mode. Two things followed from getting that wrong.

First, `virt_to_phys()` is not the conversion for it. That subtracts the direct-map offset
and is only correct for an address *in* the direct map, and the kernel image is not in the
direct map — it is mapped once, at `__kernel_start`, from whatever physical base the
bootloader chose. For `0xffffffff80xxxxxx` the wrong answer is `0x00008000xxxx_xxxx`, which
reads like a plausible low physical address and has no page behind it. `vmm_kernel_phys()`
asks `vmm_init()` for the image's base instead, and `vmm_init()` gets it by walking the
bootloader's own tables rather than assuming anything — the firmware memory map has no
entry covering physical page zero at all.

Second, and only visible on QEMU: that fetch is resolved *through the page tables*, so the
TSS's physical address also has to be a mapped linear address. Linux never notices because
its per-CPU area — where its TSS lives — sits in a region it maps at its own physical
address. `vmm_map_identity()` does the same for one page here, in the kernel's tables and
in every address space created afterwards. The diagnostic that found it is worth keeping:
the fault was a `#PF` with `CR2` exactly four bytes past the descriptor's base, which is
not an address anything in the kernel had.

The identity mapping's *intermediate* tables have to be user-accessible while its leaf does
not: the TSS page holds the kernel's stack pointers, and it lands in PML4 slot 0, which is
the slot every user mapping below 512 GiB also goes through. Marking its intermediates
supervisor-only made `walk_to_leaf()` refuse every user mapping in that slot — `elf: could
not map 0x3ff000` — which is `pt_map_supervised_range()`'s reason to exist.

### `ltr` caches the TSS, so the descriptor has to be rewritten to be reloaded

`ltr` does not read the descriptor and forget it: it caches the base, limit and attributes
*inside* the processor. Rewriting the GDT entry afterwards changes the bytes in the table
and nothing else. So `gdt_init()`'s `ltr` — which runs first in `kmain()`, before anything
knows where the image is — cached base zero, and the correct descriptor installed later was
never seen. Confirmed from QEMU's register dump: `TR =0028 0000000000000000 0000006b`, base
zero and limit correct, and a right-looking limit is the misleading part.

Two consequences, both confirmed rather than reasoned about. `tss_reload_for()` must be
called after the rewrite — but *not* `gdt_reload_for()`, which also reloads `%gs` from the
table and so zeroes the per-CPU base that lives in `MSR_GS_BASE`, turning `percpu_self()`
into a NULL dereference in the first timer tick. And a second `ltr` on a TSS that is
already busy fails with `#GP` reporting the selector — error code `0x28`, the TSS selector
itself — so refreshing the cache means writing the descriptor again (which clears the busy
bit) and *then* loading it.

### The scheduler saved six registers of a ring-3 context and dropped the other nine

The last one, and the reason the program ran for a few instructions and then faulted.

A task's saved context is either six callee-saved registers and a return address, or — for
a task interrupted in ring 3 — the interrupt frame the CPU pushed, which `iretq` consumes.
The second case was being saved as the first plus a copy of the five frame words, and that
is a complete ring-3 *frame* and not a complete ring-3 *context*: `%rax`, `%rbx`, `%rcx`,
`%rdx`, `%rsi`, `%rdi`, `%rbp` and `%r8`–`%r12` came back as whatever the *switching* task
left in them. `hello` resumed holding a `page_table_t` pointer in `%r8` and dereferenced it
— a wild pointer in the program, entirely the kernel's.

The fix is smaller than the workaround it replaces. `regs_t` — the block `isr_common()`
already hands `isr_dispatch()` — is contiguous and *ends* in the five words `iretq` wants,
so it is saved whole by pointer and restored by `isr_common()`'s own tail, minus the
dispatcher's return address. Nothing is copied, and all fifteen registers come back.

That also removed a subtlety rather than adding one: the copy version had to write *above*
`%rsp`, into the frame chain of whoever called `schedule()`, which is only safe because
that task will resume by `iretq` and never come back up that stack. Saving a pointer has no
such condition.

### And two smaller ones, both found by looking rather than by failing

- **`start.S` passed `argv` where the first parameter is `argc`.** `minx_main(argc, argv)`
  read a stack address as the argument count and a zero as the argument vector, and the
  program printed `argc=140737488350976` and `(none)`. It passed anyway, because `hello`
  handles `argc == 0` gracefully — which is the lesson: a graceful fallback in the program
  hid a wrong stack layout in the loader's caller.
- **`build_stack()` rounded the argument block's low end down to 16 bytes.** `sp = q & ~15`
  moves the stack pointer off the `argc` word whenever the block does not happen to end
  16-byte aligned, and whether it does depends on the length of the program's *name* — so
  the same kernel built `argc = 1` for one program and `argc = 0` for the next. `sp` is now
  computed aligned, first, with everything else hung off it, and the loader refuses to go
  on if the block does not land back on it.

### One bug the test could not see, found by reading the output

`syscall_dispatch()`'s last act is a check that a handler has not returned a kernel address:
`regs->rax > 0 && regs->rax >= USER_SPACE_END`. The comparison is unsigned and the frame
holds registers unsigned, so `-EBADF` is `0xfffffffffffffff7` — greater than zero, and
greater than the top of user space. **Every error return became EFAULT.** A bad descriptor
and an unmapped buffer both came back as `-14`, which is exactly the distinction the
boundary exists to make.

Every test only asked for "some error", so nothing failed. `hello` now asserts
`bad_fd == -EBADF`, and prints both errnos: `bad fd: refused with -9` and `bad pointer:
refused with -14`. It also turned out that `put_num()` wrote `23 - i` bytes where the
digits are `22 - i` — one NUL too many, invisible on a terminal, and enough to make the
tally `grep` in `tools/run-tests.sh` treat the serial log as binary and decline to report
what it found, which failed the whole suite for a reason that had nothing to do with the
kernel.

## Phase 7, where it got to

Three kernel bugs were found and fixed in this phase, and every one of them presented as
something else entirely. All three are written up in the commit messages; what follows is
the summary, so the next session does not re-derive them.

**Committed and passing** (`make test` -> 38/38, guest tally 38 passed 0 failed):

- **`fork` with copy-on-write.** A forked child is not loaded and not started: it is a
  copy of the frame `fork` arrived with, with `%rax` zero, over an address space built by
  `vmm_fork_aspace()`. Read-only leaves are shared outright; leaves holding something
  writable are duplicated along with the tables above them, so a small program pays for one
  page table per 4 MiB that holds anything writable.
- **`wait`**, and `task_unblock()`, which is the operation that means "wake something that
  is blocked" -- `task_wakeup()` on a blocked task panics, because the task is linked into
  the blocked list and a run queue refuses a task that is already on one.
- **A per-process descriptor table** and **pipes**: 16 fixed slots per task, lowest free
  number first, the console not the program's to close. A pipe is one ring, one lock (which
  *is* its wait queue's lock) and one wait queue for both ends. End of file is zero bytes;
  a full write end with no readers is `EPIPE`.
- **The ELF loader** maps whole pages and shares a page two segments both touch, taking the
  union of their permissions.

**Three kernel bugs the pipe work exposed**, all of which are in commit messages:

- `cow_copy_pt()` stored a bare frame number in a directory entry. Bit 0 of a physical
  address is the *present* bit, so the entry read as not-present and the child died fetching
  its first instruction, at the `test %rax,%rax` after its `fork` returned.
- `wait_queue_wake()` and `wait_queue_wake_all()` scanned `[head, head + count)`. `head` is
  set to zero by `wait_queue_init()` and **nothing ever advances it**, because a slot is
  retired by whichever task gets woken reclaiming it, and that can be any of them. So the
  live waiters are scattered anywhere in the array, and one wake that found nobody while
  somebody was demonstrably waiting produced a reader and a writer both blocked for ever.
  Both now scan the array.
- **A ring-3 task could not stop inside a syscall.** Such a task is saved whole and resumed
  through `iretq`; for a task blocked in a syscall that frame's RIP is the instruction after
  `int $0x80`, so resuming it delivered the *call number* in `%rax` as the syscall's answer
  and left the kernel's half of the buffer unfilled. `task_wait()` and `task_sleep_ms()` now
  clear the per-processor ring-3 marker before switching away, so a task stopping in the
  kernel comes back in the kernel with its C frames intact. The marker is also cleared on
  the way *in* -- a blocked task can come back on a processor that never saw it block --
  and clearing it on the way *out* instead wipes it for every ring-3 task, which stops the
  self tests dead just after `phase6-elf`.

**The `-smp 4` hang is FIXED.** It was the lost wake described above:
`-smp 4` goes from 3 of 8 clean runs to 6 of 6, and `make test` passes three times out of
three under KVM, which is the gate.

**Still open: `-smp 8` under TCG, about one run in four.** It is no longer a panic -- the
corrupting failures are gone, and the queue's occupancy assertion has never fired -- so what
is left is a hang, not memory damage.  And the scheduler's own invariant check now names it:

    sched: processor 5 chose the task it is already running (69, exectest, state 2);
           it will not be switched to

So the run queue handed a processor the task that processor is already running, and the
task's state was `TASK_RUNNABLE` at that point -- so it really was on the queue, not merely
reported as being so.  `list_link()`'s double-insertion panic does *not* fire, so there is
one task on one queue; the problem is that the queue's one entry is a task that is running.

**The handler is wrong, and that is the useful part of knowing it.**  `schedule()` returns
without switching, having already popped the task off the queue -- and the popped task is
marked runnable and now sits on no queue at all.  So each occurrence loses a task outright,
the machine quietly runs out of work, and nothing reports a fault.  Putting the task back
before falling through to the steal and idle fallbacks is obviously the right *handler*...

...and it was tried, and it does not fix it: over six runs at `-smp 8` the self-selection
did not occur once, while the suite got measurably worse (35-37 checks instead of 38).  So
whatever puts a running task on a run queue is doing something else as well, and masking the
symptom hides that too.  The attempt is kept at `build/sched-selfselect-attempt.patch` and
should not be applied without more understanding.

**So the state at `-smp 8`, measured on the committed tree: three of four runs clean**, and
the fourth stops early -- after 24 checks, before phase 5, so not in anything phase 7 added.
The two failures are unrelated to each other and neither is diagnosed.  `-smp 4`, which is
what `make run` and `make test` use, is green.

**Fourth bug, found on the way: the loader filled every page of a segment with a copy of
the segment's first page.** `hello` only just grew past 4 KiB of text to expose it. Nothing
faults -- the page is mapped, writable and full of valid text, just the wrong text -- and
the symptom is a `#GP` out of the `iretq` that entered ring 3, which points at the
privilege transition and not at the loader at all. See the commit message for the two
distances that were confused.

**Written, not working: signals.** `build/phase7-signals.patch`, with copies of
`signal.h` and `signal.c` beside it. A delivered signal is a frame built on the task's own
stack, the copy-on-write break taken first so a signal to a child does not write through to
its parent's stack, and delivery from the one place every return to ring 3 goes through.
Three bugs in it were found and fixed -- a hardcoded `0x2b` where the user data selector is
`0x1b` (`0x2b` is index 5, which is the TSS), and the handler's `context` argument pointing
one quadword below the saved context so `sigreturn` restored the frame's own pointers
rather than the interrupted ones.

What remains is one unexplained fault: with a handler installed at `0x401200` and the
interrupt frame's RIP written to that address -- both printed and confirmed -- the CPU
still enters at `0xa`, which is `SIGUSR1`. The saved context has been ruled out (no
`sigreturn` ever runs), and `regs_t`'s field order was measured directly by printing the
five words about to be popped: it is RIP, CS, RFLAGS, RSP, SS, as the existing comment
says. Swapping `rsp` and `ss` breaks every use of `regs->rsp`, so the current order is
right and is not the fault.

**Suggested next step:** put the delivery behind a single self-test that installs a handler,
sends one signal, and checks the count -- in its own small program, via the registry below,
rather than inside `hello`.

**`exec` is DONE.** There is a **program registry** -- a table of {name, image, bytes} that
`program_load_and_run()` and `sys_exec()` both look up by name -- and a second ring-3
program, `exectest`, which starts, prints one line, and execs `hello`.  `hello` then prints
and exits 0 in the same task, so one task checks both that a program can be started and
that a process can be replaced, and the process that exits at the end is `hello`, which
`exectest` could not have been: its own exit status is only ever what a failed `exec`
returns.  Phase 8 replaces the registry lookup with a real read of the initrd; the loader
does not otherwise change.

Three things about `exec` are not obvious and each cost a run -- all in the commit message:
the address space has to be switched before the call returns, the *live* interrupt frame has
to be patched rather than `saved_rsp`, and the task keeps its own kernel stack.  A fourth,
found on the way, is that a shared include guard in the generated image headers makes the
second program's array undeclared the moment there are two of them.

The registry is also what makes the rest of the spec's dedicated checks -- `forktest`,
`cowtest`, `sigtest` -- cheap: each is now a directory under `user/` and nothing else.  That
matters because a test living inside another program's text is a test that changes what it
is testing, and `hello` growing past 4 KiB is exactly what exposed the loader bug above.

## The two remaining failures, exactly

**Both are fixed.** The section is kept because the record of what was *wrong* is worth
more than the record of what is now right: `phase6-user`'s failure and `phase4-lockfair`'s
were two different kinds of wrong thing, and the second one is a good example of a test
whose premise a scheduler change invalidated.

`make test` -> **36 of 38 PASS**, `TEST-READY` reached, clean shutdown, no panic. The two
that fail, and neither is worked around:

### 0. `phase6-user`, progress this session -- four real bugs found and fixed

**RESOLVED.** Kept as the record of how far each fix got. What actually finished the
phase is in "What finally fixed phase 6", above.


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

**RESOLVED**, and this diagnosis was itself wrong: the frame's *offsets* were right and
the *structure* was four bytes too long, which moved every field including RSP and SS. See
"What finally fixed phase 6".


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

**`TSS.RSP0` is now right, and the fault is a VMM contradiction rather than a report
problem.** With the descriptor base fixed, the ring-3 `#DF` chain is gone: no ring-3 fault
at all, no syscall, and every interrupt reporting `cs=0x8`. So the task is not faulting --
it is not *running*. The check in `program_load_and_run()` now answers that, and it is the
sharpest thing found:

    [seg] tables=ffff800000161040 pml4=000000000513c000 ok     (three times)
    [load] A=0 B=0 E=1 P=0 pm=1 pv=1 ac=1

Read left to right: the loader mapped three pages into `t->tables`, whose PML4 is
`0x513c000`; `0x513c000` is **exactly the CR3 the CPU is running on** when the program
faults, so the right address space is loaded; `t->tables->pml4` is non-zero, its
direct-map alias is non-NULL, and the VMM is active; and yet `pt_get(t->tables, 0x40042c)`
returns 0. `build_stack()`'s own check against `t->tables` passes, so the tables are
intact a few lines earlier and gone a few lines later, with only the frame patch between.

**So this is a genuine contradiction inside the VMM, and it is where to look next:**
`pt_map_page()` writes a leaf into a table that `pt_get()` cannot find a leaf in, for the
same `tables` pointer, with `pt_invalidate()` a plain `invlpg` and
`vmm_address_alloc()`/`pmm_alloc_phys_page()` a bitmap bump allocator that cannot hand out
the same frame twice. Ruled out by measurement: `walk_to_leaf()` refusing a non-user
intermediate (its refusal message never printed); `load_segment()` failing (its error
messages never printed); the tables differing (they do not); the page-table frames being
reused (`pmm_alloc_phys_page()` never frees, and `vmm_user_page_free()` is only reachable
from two failure paths).

**FOUND: `build_stack()` zeroes the page directory.** Bracketing the walk before and after
it, for the entry point `0x40042c`:

    AFTER-SEGMENTS  L0 i=0000 e=0000000005149007
                    L1 i=0000 e=000000000514a007
                    L2 i=0002 e=000000000514d007
                    L3 i=0000 e=000000000514c005
    AFTER-STACK     L1 i=0000 e=0000000000000000     <-- the PDPT entry, gone

`build_stack()` does `memset(base, 0, usable)` where `base = phys_to_virt(t->user_stack_phys)`
and `usable = cap - 96`. The frame it zeroes is `0x514a000` -- **the page directory pointer
table**. So `t->user_stack_phys` and the PDPT are the same physical frame: the stack's
bottom page and a page table are one page of memory, and zeroing one destroys the other.

**Note also `L3 e=...c005`**: the *leaf* for the entry point has `U=0` and `W=0` -- present,
but neither user-accessible nor writable -- which is a second thing wrong with the same
mapping, and the one the CPU would hit first.

**Where to look next.** (a) The allocator handed out `0x514a000` twice: `t->user_stack_phys`
records it as the stack's bottom page, and `walk_to_leaf()` allocated it as the PDPT. Both
come from `pmm_alloc_phys_page()`, whose bitmap should make that impossible -- so check
`bit_set`/`bit_test` and whether `walk_to_leaf()`'s `pmm_alloc_phys_page()` is being counted
somewhere else, or whether `t->user_stack_phys` is being recorded from the wrong loop
iteration. (b) Fix the leaf flags: `load_segment()` passes `PTE_USER` for the image, so the
leaf *should* have `U=1`; `0x...c005` says it does not, which points at the flags reaching
`pte_make()` rather than at `walk_to_leaf()`. (c) With both fixed, `phase6-user` should
reach the syscall path for the first time.

**Two permanent checks were added rather than thrown away**, because both were silent
successes that cost hours: `program_load_and_run()` now verifies the entry point is mapped
and user-readable in the task's own tables *before* queuing the task (asked afterwards it
reports a consequence of the thing it should have prevented, and the machine is by then in
no state to print in); and `walk_to_leaf()` now says when it refuses a user mapping.

### 1. `phase6-user` -- still hanging, now much further along

**RESOLVED.** The last two things standing were the TSS layout and the scheduler saving
only six of a ring-3 task's fifteen registers. See "What finally fixed phase 6".

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

**Now changed, and not to make the number go green.** What it asserts now is the property
the lock has and the scheduler cannot take away: *no waiter was starved* — every worker
recorded all `FAIR_ROUNDS` acquisitions — and *every acquisition was inside the critical
section exactly once*, since the per-processor tallies and the holder counter agree at
`online * FAIR_ROUNDS`. That is migration-proof, and it is stronger than what it replaced:
the old assertion was unfalsifiable under work stealing, because a worker that migrated
away looked identical to a worker that was starved. The per-processor figures are still
recorded; they are just summed instead of compared.

The tempting fix — going back to counting per task — would have been weaker, and worth
naming why: a per-task count says only that every task finished, which is a liveness
property the scheduler provides on its own, with no lock involved. The per-processor
attribution was the part that said something about the lock.

## Known bugs / gaps
- **The guest hangs at `-smp 4` about one run in three, with no panic and no fault.** Two
  distinct hangs: `hello` never returns from the pipe section (the suite reports 37 of 38
  and the status it quotes is the first fork child's deliberate 7, which it reads because
  the test timed out waiting for its own task), and, about one run in eight, the run stops
  dead just after `PASS phase4-percpu`, before anything new has run. `make test` itself
  passes, so this is not a gate failure -- it is a reliability failure, and phase 7 is not
  called verified until it is gone. Full detail, and what has been ruled out, in "Phase 7,
  where it got to" above.
- A signal delivered to a program whose text spans two pages currently kills it on its
  first instruction with `#GP` out of `iretq`. Not the frame order: swapping `SS` and `RSP`
  breaks the one-page program too. See "Phase 7, where it got to".
- **The phase-5/6 blocker is FIXED and verified** -- `make test` runs the whole suite to
  the end, 38 of 38 PASS, no panic at 1 or 4 processors. Root cause, established by
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
     the scheduler`) is still unaccounted for. It has not been seen since the
     publish-before-save fix and did not appear in the `-smp 4` matrix, but it is
     not explained either, so it is not written off.

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

1. **Push access.** Every commit from phase 1 through phase 8 is local. `git push origin
   HEAD` needs a username this environment does not have. Nothing else is blocked on it.

2. **`-smp 8` under TCG still faults intermittently.** Pre-existing, unchanged by phase 8,
   and recorded rather than hidden: `-smp 4` (the default, and what `make test` uses) is
   clean, three runs in a row at 43/43. Nothing has been diagnosed for the 8-processor case
   because nothing in the tree depends on it — but it is a real open item, not a non-issue.

## Test commands
```
make test        # headless boot, in-kernel self tests, framebuffer dump; non-zero on failure
make test-host   # host-compiled allocator fuzz test (fast, no QEMU)
make run         # graphical console in a QEMU window (SMP=4 MEM=512M by default)
GUI=0 make run   # headless: serial console only
make debug       # boot paused for GDB (tools/gdb-run.sh drives it in batch)
sh tools/shot.sh build/minx.iso /tmp/shot.ppm 10   # screenshot the framebuffer

# The matrix a phase claim rests on: repeated clean runs at the default processor count.
for i in 1 2 3; do
  timeout 300 qemu-system-x86_64 -m 512M -smp 4 -no-reboot -boot d \
    -drive file=build/minx-test.iso,format=raw,if=none,id=c -device ide-cd,drive=c \
    -serial file:build/m$i.log -vga std -display none -accel tcg,thread=multi
  printf 'run %s: %s pass, %s fail\n' "$i" \
    "$(tr -d '\0' < build/m$i.log | grep -c '^PASS')" \
    "$(tr -d '\0' < build/m$i.log | grep -c '^FAIL')"
done
sh tools/type-test.sh build/minx.iso /tmp/t.log 'help
keys'                                                 # type at the console headlessly
```

## Push status
**Still not pushed. Everything through phase 9 is committed locally only.**

`git push origin HEAD` fails in this environment with
`could not read Username for 'https://github.com'` — no credential helper and no
token available, and AGENTS.md forbids putting tokens in files or remotes. Retried
at the end of phase 8 and again at the end of the four kernel fixes below; same
answer both times. Every phase since phase one has committed locally for the same
reason (see the commit "docs: record phase one push limitation").

This is the one thing in this tree that is not a technical problem and the only
thing that cannot be worked around: the work is all here, in order, with the
co-author trailer on every commit, and one `git push` away.

## Known bugs

**The shell's `.bss` bug is FIXED.  Three kernel bugs were behind it, and all three
are fixed and committed.**

The first theory on this page — that `load_segment()` conflated a page's distance from the
segment's start with the segment's offset into the page — was **right**, and the note that
said it was wrong was wrong about that.  What was true, and what is still true, is that
rewriting the whole function broke `fstest`; the two are not the same claim.  Three
separate defects, each invisible on its own:

1. **`file_t` had a reference count that nothing ever moved.**  `vfs_open()` set
   `refs = 1` and `vfs_close()` freed the file whatever the count said, while `dup`, `dup2`
   and `fork` all made a second descriptor name the same `file_t` without accounting for
   it.  `int fd = open("/dev/console"); dup2(fd,0); dup2(fd,1); dup2(fd,2); close(fd);` —
   which is exactly what `/sbin/init` does — freed the file out from under all three.  The
   freed block stayed readable long enough that init kept working, and the failure turned up
   later, as `EBADF` on a write to the console in a child that had inherited the descriptor.
   Fixed by making `vfs_close()` decrement and free at zero, and retaining in `dup`, `dup2`
   and `fd_table_copy()`.
2. **`exec` did not carry the program break across the image swap.**  `program_exec_into()`
   moved the page tables, the frame, the stack and the saved `rsp` from the scratch task but
   not `brk`.  An `exec`'d process kept the previous program's break — zero for one the
   kernel started — so `sbrk(0)` answered zero, the allocator took that as the base of its
   heap, and the first `strdup()` wrote its string over a hundred kilobytes of `.bss`.
   That is what put pointers into `sh`'s variable table.  A program started by the *kernel*
   is loaded onto the task itself, where `elf_load()` sets the break directly, which is why
   every test in the tree passed and `sh` — the one program init starts with `exec` — did
   not.
3. **The loader copied file bytes into the part of a segment the file does not cover.**
   `load_segment()` decided whether a page held any of the file with arithmetic on `filesz`
   alone, which says "copy `filesz` bytes" for as long as `filesz < page_size` — true of
   every writable segment a linker produces.  Each `.bss` page was copied from an offset
   the segment's file data ended long before, and for a segment at the end of the file,
   from past the end of the image buffer.  About 0x170 bytes of whatever the kernel's
   memory held were dropped at the start of every `.bss` page, silently, with the loader
   reporting success.

All three are committed separately with the reasoning in the commit messages.  `sh` now
boots from a normal image, prints its banner and shows a prompt.

**`sh <script>` produces nothing: the program starts and its output is lost (open).**

`shtest` writes a script, forks, `dup2`s a file onto descriptor 1, `execve`s `/bin/sh` with
the script as `argv[1]`, and reads the output back.  The output file exists and is **zero
bytes**; the script itself is present with its correct 29-byte length; the script exits 0;
and the shell's prompt, banner and every line of the script are absent.

Much of this has been established with GDB, and the answer is no longer "the child does not
start":

- **The exec works completely.**  With a breakpoint on sh's own entry point, it is reached,
  in ring 3: `Breakpoint 2, 0x000000000040ac90`, `$cs = 0x23`.  Nothing is wrong with the
  loader, the swap or the return.
- **The task is set up correctly.**  At the end of `program_exec_into()` for `/bin/sh`,
  task 30 has `state = 3`, `started = false`, `saved_in_user = 1`, `saved_rsp =
  0x7fffffffefa0` (a user address), `user_frame` pointing into the direct map of the new
  stack's top page, and `brk = 0x2403000`.
- **`do_exec()` patches the frame it is about to return on.**  At `kernel/lib/syscall.c:766`
  the frame is a valid ring-3 frame on a kernel stack — `cs = 0x23`, `rsp = 0x7fffffffec68`,
  `rip = 0x406617` (the address in `shtest` that called `execve`) — and `entry = 0x40ac90`.
- **Every write reaches the kernel.**  A breakpoint on `syscall_dispatch` filtered to
  `SYS_WRITE` shows, in order: `WRITE fd=3 task=29` (shtest writes the script), `FORK
  task=29`, `WRITE fd=2 task=30` and `WRITE fd=1 task=30` (the child's own probe, before the
  exec), `EXEC task=30`, then `WRITE fd=1 task=30`, `WRITE fd=2 task=30`, `WRITE fd=3
  task=30` — the shell writing to all three of its descriptors after the exec.
- **And none of them produce output.**  Not on the serial port, and not in the file, which
  is read back as zero bytes.  Both write paths — the console (`fd` 2, `FD_CONSOLE`) and a
  regular file (`fd` 1, `FD_FILE`) — lose the data for this task, while task 29's writes to
  the console come through.

Ruled out along the way: the shell (its entry point is reached); `exec` (it completes and
the task is correct afterwards); the script argument (passing none changes nothing);
`dup2` (removing it changes nothing); the loader; and the `.bss` bugs above.

**Not diagnosed.**  The next move is to find why `fs_sys_write`'s two back-ends drop the data
for task 30 and not for task 29 while the syscall itself is entered — the return value of
those writes has not been read yet, and that is the first thing to get.  `finish` inside a
GDB `commands` block does not survive the block, so the probe has to break at the *next*
`syscall_dispatch` and read the `rax` left behind by the previous one, or break in
`fs_sys_write` by address rather than by name (it is not currently a symbol in the
kernel's debug information, which is worth fixing).

**The ramfs node pool is exhausted, intermittently (open).**

`ramfs: no node left for "shtest.sh"`, and during `libctest` in one run.  `RAMFS_NODES` is
2048 in `kernel/lib/ramfs.c`.  A node is released only by `ramfs_unlink()`, never by
`close()` — which is correct POSIX, since closing a file does not remove its name — so
every file a program creates and closes without unlinking keeps its inode for the rest of
the boot.  With 2048 inodes and a self-test that starts dozens of programs, some of which
create files under `/tmp`, that is reachable.  Whether the tests are leaking more than they
should or the pool is simply small has not been established; the first thing to count is how
many nodes are in use when `libctest` fails.

**`PATH is set before main()` fails intermittently (open).**

Seen once in four post-fix runs, in `libctest`, and it is the whole of that run's failure
(43/44).  `__libc_start_main()` reads `envp = argv + argc + 1` and installs
`startup_env` when `*envp` is NULL, so for a kernel-started program `PATH` is always set —
which is why an intermittent failure here is suspicious rather than explained.

Three libc and loader bugs found along the way, all fixed and all committed separately:
`printf("%d", 0)` wrote a NUL byte instead of `"0"`; the argument block was laid out
downward from its top, so `argv[0]` was written where `argv[argc]` belongs and every libc
program read eight characters of `PATH=/bin:/usr/bin` as the end of its argv; and
`fallback[]` in `build_stack()` was declared inside the `if` that used it, so its sixteen
bytes were handed to the address arrays declared after it and the loader asked `strnlen()`
for the length of a null pointer.

## Next

**First: the lost writes above.**  Everything upstream of them is now proven correct --
the exec completes, the task is set up right, the new entry point is reached in ring 3 and
every write syscall is entered.  So the question is narrow: why do a console write and a
file write both come back empty for the task that forked and exec'd, and not for the task
that forked it?  Read the return value of those writes first.

**Then: reliability.**  The suite is one bad run in three, and the failing check moves
around.  The two candidates found are the ramfs node pool (2048 inodes, released only by
`unlink`) and `PATH is set before main()`; counting nodes in use at the point `libctest`
fails is the cheapest way to tell whether the first is the same problem as the second.

**Then: `sh`.**  `job control` beyond the table (`fg`/`bg` currently record and report but
do not move a job's process group to the foreground), then `shtest` to green, then flip
`WITH_SHELL` to 1 by default.

Then phase 10: the coreutils (`ls`, `cat`, `grep`, `sort`, `uniq`, `wc`, `head`, `tail`,
`tr`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `echo`, `true`, `false`, `date`, `env`, `kill`),
`grep -r`, an editor, and the rest of `/proc`.

`gdb` is installed and works against this kernel: start the guest with
`qemu-system-x86_64 ... -s -S -daemonize`, then
`gdb -batch -q build/kernel.elf -ex 'target remote :1234' -ex 'break syscall_dispatch' -ex continue`.
Two things to know: `arg(regs, n)` is not visible to GDB, so read `regs->rdi` and friends
directly; and a stale guest still holding port 1234 will be attached to instead of the new
one, which is `fuser -k 1234/tcp` away.