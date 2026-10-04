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
