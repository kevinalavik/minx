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