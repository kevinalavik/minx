# STATUS

Current phase: 4 (SMP) — phases 1 to 3 VERIFIED

## Phases
| # | Phase | State | Evidence / command |
|---|-------|-------|--------------------|
| 1 | Toolchain & boot, framebuffer console | VERIFIED | `make test` → 17/17 PASS, framebuffer dump captured; `make run` boots to a graphical console at 1280x800 with 160x50 text cells. Serial log shows the memory map, RSDP, `SMP: 4 CPUs`, initrd and heap. Panic path verified separately (register dump + stack trace on both consoles) while fixing bring-up bugs. |
| 2 | CPU core, keyboard into console | VERIFIED | `phase2-acpi` (4 processors with distinct LAPIC ids, IOAPIC at 0xfec00000), `phase2-interrupts` (IOAPIC owns all 16 lines, 8259s stay masked, a mask round trip changes the chip's own bit, and both the LAPIC timer and the 8253 really fire over 120 ms with interrupts on), `phase2-keyboard` (8042 self test, scancode set 1, reachable on IRQ 1), `phase2-input` (event ring in and out in order). Typed keys reach the graphical console: `tools/type-test.sh` types `help`, `keys`, `irqs`, `uptime`, `meminfo` and all five answer. |
| 3 | Memory | VERIFIED | `phase3-pmm` (frames page-aligned, mapped, distinct, hold their contents, free count survives an alloc/free round trip), `phase3-pmm-contiguous`, `phase3-direct-map` (bottom/middle/top of RAM translate to themselves; one frame past the last usable frame is not mapped), `phase3-pmm-churn` (4096 allocations through 64 slots, no frame handed out twice). `meminfo` reports 68 MiB heap with 87 KiB used. |
| 4 | SMP | NOT STARTED | `percpu_start_ap()` exists and reports why it declines; only the BSP runs. `lapic_ipi_init()` / `lapic_ipi_startup()` are written against the APIC window phase 3 now provides. |
| 5 | Scheduler & kernel threads | NOT STARTED | |
| 6 | Userspace entry | NOT STARTED | |
| 7 | Processes, signals, pipes | NOT STARTED | |
| 8 | VFS + ramfs + initrd + devfs | NOT STARTED | |
| 9 | TTY + libc + sh | NOT STARTED | |
| 10 | Coreutils + procfs | NOT STARTED | |
| 11 | Hardening / stress | NOT STARTED | |
| 12 | Polish | NOT STARTED | |

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
- **Self tests**: seventeen in-kernel checks (`minx.test=1`) plus a
  host-compiled allocator fuzz test (`make test-host`).

## Known bugs / gaps
- Only the boot CPU is running; `percpu_start_ap()` reports why and declines.
  This is the whole of phase 4.
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
