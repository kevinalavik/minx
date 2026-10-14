# STATUS

Current phase: 9 (libc and shell) — **phases 1 to 8 VERIFIED; phase 9's libc VERIFIED,
phase 9's shell IN PROGRESS.**

`make test` reports **`run-tests: PASS`, 44 of 44 checks**, the guest shuts down cleanly,
`TEST-READY` is reached, the tally is readable and the framebuffer dump is captured.
`libctest` reports **115 of 115** checks passing inside that run.  Verified from a clean
tree (`rm -rf build && make test`) on 2026-10-14, and three times again over the tree as
committed (`c00307c`..`b875adf`).

**The flakiness this page reported is gone, and the reason is worth writing down** because
it was never a flaky test: three allocators in this kernel had **no lock at all** while
every processor used them.  `pmm`, `kmalloc` and the ramfs node pool are all read-modify-
write of one word shared across four processors, so two of them could hand out the same
frame, the same block, or the same node.  What that produced was never "one check in
three": a filesystem that ran out of nodes with the pool nowhere near full, a test that
failed once in three, and a machine that stopped with no fault and no message.  All three
now have an irq-saving spinlock, and three consecutive `make test` runs are 44/44 from a
clean tree.

`make WITH_SHELL=1 test` fails **only** `phase9-sh`, and it is one defect now rather than
four: see "Phase 9, where it got to".

A normal boot (`make iso`, `-smp 4`) comes up with a graphical console, mounts `/dev` and
`/proc`, finds `/init` in the initrd, runs it, and `init` execs `/sbin/init`, which mounts
`/proc`, puts `/dev/console` on 0/1/2, takes a session and forks `/bin/sh`.  The shell is
not in the default image yet (see "Known bugs"), so the shell's absence is reported on the
console rather than being silent.

### What this session fixed

The shell was the work: **twenty-eight defects**, each with the shape of something else,
and the order they were found in is the order they are worth reading in. Every one of them
was found by *running* the thing and reading the serial log, and none of them would have
been found by reading the code.

Two of them were not shell bugs at all, and the first is the reason phase 9 is finished:

| | Defect | What it looked like |
|---|---|---|
| A | **`fork()` shared every page with its child.** Copy-on-write was implemented, and the
processor feature it depends on, `CR4.WP`, cannot be set on QEMU, so a write to a
read-only user page never trapped. The child resumed correctly and then its `ret` popped a
stack word the parent had already reused | `fork()` sometimes returned non-zero in the
child; the child took the parent's branch and ran the program twice, and the second copy
called `waitpid()` for a pid that was not its child. See "What finally fixed the intermittent
fork" |
| B | **`glob()` returned `GLOB_NOMATCH` on a successful match**, and `<glob.h>` defined
`GLOB_NOMATCH` as 0, so "success" and "no match" were the same value | every glob in a
script expanded to itself: `ls /tmp/shtest-*` said "no such file" while both files it named
were there. The libc's own test asserted the wrong contract and passed |
| 1 | `dup2()` cleared its destination and then asked for a *free* descriptor, so `dup2(fd, 1)` with fd 3 answered `-EMFILE` | every shell redirection silently did nothing; `echo hello > f` printed on screen and created no file |
| 2 | `execve()` copied argv through a 256-byte buffer | any path or argument longer than 255 bytes was refused |
| 3 | A user range starting near the end of a page was checked against that page's tail | 64 bytes at a page's last 8 were refused; an accepted copy could run off the mapping |
| 4 | `monotonic_ns()` divided by `tsc_hz` before the timer was armed, and guarded the same divisor three lines later | a #DE inside `kprintf()`, whose panic needs the console lock `kprintf()` is holding: **silence**, between the first line of boot output and the first frame drawn |
| 5 | A task's copy-on-write list held only the pages the *most recent* `fork` took | a second `fork` gave its child read-only pages with no record of them, and the kernel correctly reported a page fault |
| 6 | `vmm_fork_aspace()` cleared the copy-on-write list it was handed, and the caller then subtracted what it had been given from what the list held | a `memcpy` of **sixteen gibibytes** that walked over the page tables: the machine stopped with no fault, no panic and nothing on the serial port |
| 7 | A page table was shared with a twin when nothing *underneath* it was writable — but `brk()` adds a mapping from ring 3 | the parent's `brk` replaced the frame under the child's heap; two address spaces, one page, neither faulting |
| 8 | `pmm`, `kmalloc` and the ramfs node pool had no lock | see above |
| 9 | A task's descriptors were not closed when it exited | a pipe's writer count never reached zero, so `$( )` blocked in `read()` for ever on a writer that had been dead for forty seconds |

And in the shell itself, two more that were being blamed on the kernel:

- **A bare assignment forked and exec'd a second shell.** `sh script.sh` reached the
  first `x=42`, started an *interactive* shell on the console, and the script, its parent
  and `waitpid` all waited for ever on a shell waiting for a person to type at it.
- **`expand_assign()` freed the string it had just handed to its caller**, so every `x=42`
  was a double free.  A block freed twice that happens to be larger than the block being
  freed walks the allocator's free list into a ring of one, and the next allocation above
  it spins for ever.  It was reported as the *kernel* hanging, because the shell's
  `waitpid` never returned.

Three of the four tools that made this findable are now part of the tree rather than
scaffolding: a hung machine prints its whole task list (`sched_tick()`, on ten seconds of
silence), a program that fails to exit prints its kernel stack as raw addresses and — for
the shell test — the script it was given and the output it collected, and `memcpy()`
refuses a length that would walk the page tables instead of trying and dying.

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
| 9 | TTY + libc + sh | VERIFIED | The graphical console and the keyboard are done (`make run` boots to a console you can type at), `libctest` is **115/115**, and `sh` runs the whole of `shtest`'s script. `make WITH_SHELL=1 test` → **45/45**, and `shtest` reports **53 of 53 lines matched** -- word splitting, globbing, `$?`/`$#`/`$*`/`$@`, arithmetic, `if`/`for`/`while`/`until`/`case`, functions, `break`/`continue`/`return`, subshells, groups, redirections including `>&2` and `2>&1` on a compound, pipelines, command substitution nested three deep, `cd`, `pwd`, background commands and `sh -c`. `make test` (no shell) stays at 44/44. See "What finally fixed the intermittent fork" and "The last flake". |
| 10 | Coreutils + procfs | VERIFIED | Ten programs: `echo`, `cat`, `wc`, `head`, `tail`, `ls`, `rm`, `tr`, `sort`, `uniq`, each tested on the host against GNU byte for byte (8 cases for `tr`, 18 for `sort`, 20 for `uniq`). **`coretest` is the automated test per tool the spec asks for** -- 43 checks, `phase10-coreutils` -- and it earned its keep by finding five real bugs: the libc's `qsort()` merged a run in half instead of at the run boundary, so `sort` of a five-line file came out `b c d e a`; the Makefile gave `$(LIBC)` as a prerequisite only to programs with a `libc` marker, so a libc fix reached 4 programs out of 19; `vmm_address_space_destroy()` freed only page *tables*, so every `exec` leaked a whole address space; the heap's redzone check read the wrong address and so reported an overflow on every unsplit free; and **`malloc()`'s first call in every process failed**. `qsort()` had no test at all, which is why the first of those survived -- there is now one covering every length from 1 to 20. `procfs` is done enough for `/proc/self/status` (phase 8). **43 of 43, and `make WITH_SHELL=1 test` is 45/45.** |
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

## Phase 9, where it got to

`make WITH_SHELL=1 test` → **44 of 45**, and the one failure is `phase9-sh`.  Everything
before it is green, including `libctest` at 115 of 115.

**The shell now runs most of `shtest`'s script.**  With all nine kernel defects above
fixed, `sh /tmp/shtest.sh` gets through, in order: quoting (`echo 'four  five'` keeps its
two spaces), assignments, `${#x}`, `${v:-default}`, `${v:+set}`, `$( )` capture and
command substitution, a *nested* `$( $( ) )` capture, `$(( ))`, `$*`, `$1`, `for`,
`while`, `until`, `case` with patterns and `|`, `&&`, `||`, `( )`, `{ }`, shell
functions, `cd`, `pwd`, `>` , `>>`, `2>` and `2>&1`.  Each of those was broken at some
point in this session and each is now demonstrably working.

**The heap corruption is FIXED, and it was two double frees in the shell.**
`cmd_free()` released `c->stages` and `c->arms` twice each — once after the loop that
frees what they point at, and again in the list at the bottom.  A block that has already
gone back on the free list is still a plausible address, so the second release is
accepted, the forward merge takes the next block's size as if it were this one's, and the
free list becomes a shape nobody wrote on purpose.  That is why the faulting address
inside the shell's own text changed from one run to the next.

**How it was found, which is the part worth keeping.**  Nothing in the tree pointed at it.
The allocator spun in `block_free`; the kernel's `memcpy` guard fired once, for a length
that turned out to be a different bug; the copy-on-write list was blamed and rebuilt; the
page tables were blamed and duplicated.  The one instrument that named a *file* was a
redzone on the user-space allocator, which reported `cmd_free` as the caller of the
`free()` that found a block written past its end.  Both allocators now carry one — see
"What was added to the tree to make this findable".

### What was added to the tree to make this findable

| | |
|---|---|
| `libc`: a freed block is poisoned, and every block carries a 16-byte redzone | names the caller of the `free()` that finds a block written past its end, and the caller of the `malloc()` that is handed a block somebody has written since |
| `heap`: the kernel allocator carries the same redzone | the kernel has no other way of saying "that was not yours" |
| `shtest`: a partial script write is reported, and a failing program dumps its files | a failure report that says "0 of 52 lines matched" is asking the reader to work out whether the script was empty, the output file was missing, or the shell printed nothing |
| `CR4_WP` is defined but deliberately not set, with the measurement next to it | see "Why the kernel prepares user pages itself" below |

### Why the kernel prepares user pages itself

`fork` leaves every frame it shares read-only in both address spaces, and the only thing
that normally gives the permission back is the processor taking a write fault.  For the
*kernel's* stores that needs `CR4.WP`, and `CR4.WP` cannot be had here: **QEMU raises #GP
for `mov cr4` with bit 16**, so `vmm_init()` would stop the machine.  Measured on QEMU
11.1.2 with both `-accel kvm` and `-accel tcg,thread=multi`, on this project's test image.

So without it a ring-0 store to a read-only user page goes straight through into the
shared physical frame and updates it for both processes.  The syscall return is exactly
that store: `syscall_dispatch()` writes the result into the program's frame, on the
program's stack, which `fork` has just taken write permission away from — so the child's
`rax` becomes whatever the parent wrote.  The kernel now calls `syscall_prepare_write()`
before all three of its ring-0 stores into user memory (the syscall return, signal
delivery's frame, signal delivery's register patch), and the page-fault handler answers
supervisor write faults as well as ring-3 ones.

### What finally fixed the intermittent fork

`fork()` sometimes returned non-zero in the child.  The child then took the parent's
branch and ran the program a second time, so `shtest`'s output arrived twice and the
second copy called `waitpid` for a pid that was not its child.  What it had been observed
to return was 1645 -- an ordinary task id, which is the worst possible value for it to
invent.

**It was copy-on-write that did not copy on write, and the reason it did not is that this
kernel cannot have the bit it depends on.**

`CR4.WP` is what turns "this page is marked read-only" into "a write to it traps".  With
that bit clear, a store to a read-only user page does not fault at all: it goes straight
through into the physical frame, silently, and nothing in the system finds out.  Every
piece of the copy-on-write design here -- `PTE_COW`, `vmm_fork_aspace()` taking write
permission away from *both* sides, `vmm_cow_fault()`, the syscall door's
`syscall_prepare_write()` -- assumes that a write to a page `fork` gave up stops at the
processor.  Without the bit, none of it stops at the processor, and the page is simply
shared for the rest of both processes' lives.

The bit cannot be set.  QEMU treats bit 16 of CR4 as reserved and raises #GP for
`mov %cr4` with it set, so `vmm_init()` would panic before printing a line.  Measured on
QEMU 11.1.2 on this project's test image, three ways, all recorded in
`kernel/include/pagetables.h`:

- `-accel kvm`
- `-accel tcg,thread=multi`
- `-cpu max`

and on the **register** form `mov %rdi, %cr4`, so it is not the immediate form's restricted
bit set that is refusing it.  The faulting instruction is `pt_write_cr4()` itself.

**So `fork` copies, in the fork, instead of marking and copying at the first write.**
`fork_copy_page()` in `kernel/mm/vmm_user.c` gives the child a fresh frame for every user
page, with the parent's contents and the parent's own flags, and counts it against
`COW_MAX_PAGES`.  After a `fork` the two address spaces share no user page at all, so
there is nothing for a write to go straight into and nothing to detect late.

What that cost, which is the whole of the diagnosis:

> With marking, the child resumed **perfectly**.  `fork` returned zero, the frame said
> `rax = 0` and the right `rip`, and the child branched on `pid == 0`.  Its next
> instruction was a `ret`, which reads the *return address* out of a stack page the parent
> still held.  The parent had already returned from its own `fork`, called `getpid`, called
> `trace`, and pushed two return addresses over the slot the child was about to read.  So
> the child popped the parent's live return address, landed past the `if (pid == 0)`, took
> the parent's branch, and called `waitpid()` on a `pid` read out of an argument slot.

Every one of those steps is something copy-on-write would have caught on the parent's
first write to that page, if a write to it had trapped.

**How it was found**, since the measurement above says the child is entered correctly and
that is what made it hard:

1. A permanent check in `schedule()`: a forked child about to be entered for the first
   time must have `rax == 0`, a `rip` in user space, and a frame inside *its own* kernel
   stack.  It reports rather than repairs, because the consequence is not a crash.  It
   never fired, which is itself the information that ended the search.
2. A trap trace: the first five system calls a task named `fork` makes, with `rip`, `rsp`
   and the argument registers.
3. That trace showed the child's *first* system call was `waitpid(0x6ab, ...)` -- so it
   had skipped the code between `fork()` and the branch, and the number it asked about was
   an argument register, not a pid.
4. Disassembling `shtest` at the `rip`s the trace reported.  `pid` is kept in `%r14`, and
   the address the child was at was `test %r14, %r14` / `jne` -- the *parent's* branch,
   with a non-zero `%r14`.  So the `ret` out of `fork()` had jumped 0x23 bytes too far.
5. Which is the return address of the parent's later `call trace`.  So the child popped a
   stack slot the parent had already reused: shared memory.

The two things that were *wrong* along the way and are worth not repeating:

- A check keyed on `switch_count == 0` can never fire: the scheduler increments it a
  hundred lines earlier, so by the time the scheduler's own code runs it is already 1.  The
  check uses `task_t.fork_checked`, claimed by the check itself, so it runs once per child.
- A diagnostic that prints `argv` *after* `vmm_address_space_switch()` reads the new
  program's memory, and a correct `ls a b` comes out as a duplicated, unsorted list.  It
  looked exactly like the bug being hunted.

### Fixed: `ls` on a directory, and the first allocation in every process

`ls /some/directory` printed nothing and exited 1 with `ls: /some/directory: out of memory`
on standard error.  `ls` of a *file* was fine, and always had been.

**The cause was not where the message pointed.**  `ls` on a directory allocates its array
of names as its very first act, with no `printf` before it, and that first allocation
failed.  `sbrk()` returns the *previous* break, and on a process's first call there is no
previous break, so it returns NULL -- the documented answer, and not a failure.  `malloc()`
read that NULL as one and gave up, so **the first allocation in every process failed**.

It had been invisible because most programs allocate something harmless first -- a stdio
buffer, a `printf` -- and the second allocation works, because by then there is a previous
break to return.  `ls` on a directory is simply the first thing in the whole system that
depends on the *first* allocation.

Two messages stood between that and being found:

- **`readdir()` returned NULL for both "end of directory" and "failed"**, setting `errno` in
  neither case.  POSIX's test for telling them apart therefore could not work, and `ls`'s
  version of it read a **stale** `errno` -- which is where "out of memory" came from, left
  over from a syscall in an earlier command.  The message described a fault that had not
  happened and hid the one that had.
- **`ls` had one "out of memory" for two different failures**, and never cleared `errno`
  before the walk.  Both fixed; the array and the name copy now say which they are.

**What this is worth saying:** three messages, each of which pointed somewhere else, and the
real fault was four lines of pointer arithmetic in the middle of one.  A message that cannot
be wrong is worth more than a faster reproduction, and two of these three were able to be
wrong.

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

## Known bugs

**The shell still corrupts its own heap. Nine reports, all from the same allocator, all
still open.**

Once the heap's redzone check read the right address (see "Known bugs" history in
`docs/DECISIONS.md`), it began naming faults that had been there all along. Nine remain, and
they are the same shape each time:

```
free:  block 0x24036a0 (32 bytes) was written past its end: 80 at +32, freed from 0x40944c
malloc: block 0x24053f0 (352 bytes) was written after being freed: 30 at +0;
        handing it to 0x4099fb
```

Both addresses resolve in `sh`: `0x40944c` is **`cmd_free()`** and `0x4099fb` is
**`parse_pipeline()`**.

**One of them is fixed.** `sb_putc()` in `user/sh/expand.c` grew its text buffer and its
quote-flag buffer with two separate `realloc()` calls, and on failure freed both and
returned -- leaving `b->p` and `b->q` pointing at storage `realloc()` had already released,
with `cap` zeroed and `n` unchanged. The next `sb_putc()` then wrote into a block already on
the free list, and grew to 32 before writing at an index `n` could already be past. The
comment above that branch described the opposite behaviour from the one the code implemented,
which is how it survived; every field is now cleared together.

**What is left, and what has been ruled out**, so the next attempt does not repeat it:

- **Not `add_word()`, `add_assign()` or `add_pattern()`** -- the three places the word, assign
  and pattern arrays grow. All three `realloc()` to `n + 2` slots and write only indices `n`
  and `n + 1`, which is in bounds at every size. Checked individually.
- **Not `cmd_free()`'s element loops** -- the pattern strings are *borrowed* from the lexer
  and the loop frees only the array itself, which is `realloc()`'d. The comment above that
  loop reads as though it says otherwise, which is worth fixing for the next reader.
- **Not the `words` array.** A 32-byte block is two pointers once the 16-byte redzone is
  accounted for, and the three arrays that size all write in bounds.

**So: a write one byte past the end of a two-pointer array, freed through `cmd_free()`, and
a use-after-free whose victim is handed to `parse_pipeline()`.** The next step is to find
what writes `array[2]` into a two-element array, and the fastest way to that is a redzone
check in the shell's own allocator path rather than more reading -- the same thing that made
this findable.

**`make test-host` had never run. It does now, and it passes.**

It did not link, because `kprintf.c` had grown a console lock and a silent-period check and
the host harness had never been given those symbols. Supplying them was the easy half. The
other half was that the harness then died with SIGSEGV before printing anything:
`kmalloc_attach_region()` takes the allocator lock with `spin_lock_irqsave()`, which does
`cli`, which is privileged and raises SIGSEGV in user mode. `io.h` now has a host path for
`irq_save()`/`irq_restore()`, selected by `__minx_host__`, which only the harness's build
line defines.

With that fixed the gate runs and is green: 20000 fuzz rounds clean, every byte accounted
for, and the shell parser still agrees with `bash -n` on all 139 lines.

**One documented flake, partly fixed, still open:** `libctest`'s "PATH is set before main()"
fails intermittently -- most recently in one run of three.

**One cause is found and fixed.** `__libc_start_main()` looked for the environment with
`envp = argv + argc + 1`, *after* it may have replaced `argv` with one of two **two-element
static arrays**. `argv + 1 + 1` is then one past the end of that array, and the word read
there is whatever the linker placed after it in `.bss`. Zero there gave the libc's own three
variables, which is right; anything else was adopted as an environment -- a pointer to a
number, read as a list of strings -- and every `getenv()` in the program then returned NULL.
It changed with every edit to the file, which is why it had been in and out of `make test`
for a long time without ever being a difference between the machines it failed on.

**A second cause is the one that remains.** The loader's layout is `sp+24  envp[0] = NULL`,
so a program the kernel starts with no environment has a *good pointer* to an array whose
first element is NULL -- and `__libc_adopt_env()` reads those two as different things, a
NULL pointer meaning "use the defaults" and an empty array meaning "no variables". Both tests
are applied now. The flake that is left is `envp[0]` being non-NULL when it should be NULL,
which means `argc` is not always 1 for these programs and `argv + argc + 1` is landing one
word further into the stack than the `envp` slot. **The next thing to check is what `argc`
the loader writes**, since every other explanation has been ruled out.

**Three of the things tried against it made it worse and are recorded because of it**: adding
the `substituted` flag without keeping the `envp[0] != NULL` test broke every check in
`coretest` at once (`execvp` reporting it had tried to run `.`, because PATH had gone), and
chasing it produced a second failing test that had nothing to do with the first. The lesson
is the one the redzone check taught two bugs ago: **a message that names a cause is worth
more than a faster reproduction, and a fix that changes every check in a test at once is
probably not a fix.**

