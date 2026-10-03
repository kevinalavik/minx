# STATUS

Current phase: 3 (memory) — phases 1 and most of 2 done

## Phases
| # | Phase | State | Evidence / command |
|---|-------|-------|--------------------|
| 1 | Toolchain & boot, framebuffer console | VERIFIED | `make test` → 9/9 PASS, framebuffer dump captured; `make run` boots to a graphical console at 1280x800 with 160x50 text cells. Serial log shows the memory map, RSDP, `SMP: 4 CPUs`, initrd and heap. Panic path verified separately (register dump + stack trace on both consoles) while fixing bring-up bugs. |
| 2 | CPU core, keyboard into console | MOSTLY DONE | ACPI RSDP/RSDT/MADT parsing verified (`phase2-acpi`): 4 processors with distinct LAPIC ids and an IOAPIC at 0xfec00000. 8259 remap and mask readback verified (`phase2-pic`). Input ring verified (`phase2-input`). The PS/2 driver negotiates set 1 and registers on IRQ 1. **Blocked: typed keys do not reach the CPU** — see below. |
| 3 | Memory | NOT STARTED | The blocker for finishing phase 2. |
| 4 | SMP | NOT STARTED | |
| 5 | Scheduler & kernel threads | NOT STARTED | |
| 6 | Userspace entry | NOT STARTED | |
| 7 | Processes, signals, pipes | NOT STARTED | |
| 8 | VFS + ramfs + initrd + devfs | NOT STARTED | |
| 9 | TTY + libc + sh | NOT STARTED | |
| 10 | Coreutils + procfs | NOT STARTED | |
| 11 | Hardening / stress | NOT STARTED | |
| 12 | Polish | NOT STARTED | |

## The blocker on phase 2, in detail
Interrupts are configured correctly on every side the kernel controls, and the
machine still takes none of them. Established by measurement, not inspection:

- `if=1` in RFLAGS, `picmask=0xfffc` (IRQ0 and IRQ1 unmasked, read back from the
  chips, not from a cache).
- QEMU's `info pic` shows `pic0: irr=03` — IRQ0 and IRQ1 pending and unserviced.
- QEMU's `info lapic` shows `LVT0 ... masked ExtINT`. LVT0 is the local APIC's
  connection to the 8259s, and Limine masks it before handing off (see
  `lapic_configure_bsp()` in Limine's `common/sys/lapic.c`).

The mask bit can only be cleared by writing the LAPIC's LVT register over MMIO,
and the bootloader's page tables deliberately do not map MMIO: the higher-half
direct map covers RAM only, and there is no identity map either. So the LAPIC is
unreachable until the kernel owns its own page tables. `vmm_find_mapping()`
probes for a usable window and correctly reports that there is none.

Hence the order is: phase 3 (PMM + VMM, which maps the APIC windows) → finish
phase 2 (unmask LVT0, LAPIC timer, typed keys) → phase 4 (SMP, which needs the
same window for INIT-SIPI-SIPI).

The LAPIC, IOAPIC and PIT code all exist and are written against that window; the
per-CPU array, the PIT time base and the keyboard driver are all in place.

## What exists today
- **Boot**: Limine 12.9.1, hybrid BIOS+UEFI ISO, higher-half kernel at
  `0xffffffff80000000`. Memory map, framebuffer, RSDP, MP info and the initrd
  module are all taken from the bootloader's responses.
- **Console**: own framebuffer renderer (`kernel/drivers/fb.c`) plus an
  ANSI/VT100 terminal (`kernel/drivers/console.c`) with SGR colours, 24-bit
  colour, bold/inverse attributes, scrolling regions, cursor movement and
  erase. Everything is mirrored byte-for-byte to COM1.
- **Font**: the 8x16 `lat9-16.psf` from the Linux `kbd` package, converted to a
  C array at build time by `tools/psf2c.py`. No font scaling: a 1280x800
  framebuffer yields a 160x50 terminal.
- **CPU core**: GDT (ring 0/3 + 64-bit TSS), TSS with IST stacks for #DF and
  NMI, IDT with all 32 exceptions plus the 16 legacy IRQ vectors, and an
  exception reporter that decodes page-fault error codes and dumps registers
  and a stack trace to both consoles.
- **ACPI**: RSDP, RSDT or XSDT, MADT (including x2APIC and LAPIC-override
  entries), FADT discovery. `minx.verbose=1` dumps raw table bytes.
- **Interrupt controllers**: 8259 remap and full masking, LAPIC and IOAPIC
  enumeration and programming, IRQ routing with per-line handlers, counters and
  names. Gated on the APIC window being mapped, which phase 3 provides.
- **Time**: TSC-calibrated delays, PIT-gate calibration of the LAPIC timer with a
  TSC cross-check, the 8253 as a 100 Hz IRQ0 time base, and an RTC reader for
  the wall clock.
- **Input**: a lock-free SPSC input queue, a PS/2 keyboard driver that
  negotiates scancode set 1 (or asks the 8042 to translate set 2), decodes
  shift/ctrl/alt/meta/caps/numlock and pushes key events, and a console
  command loop that echoes them and answers `help`, `meminfo`, `irqs`, `keys`,
  `cpus`, `uptime`, `clear` and `reboot`.
- **Panic**: prints the message, a full GPR dump and a kernel stack trace, then
  writes 1 to QEMU's isa-debug-exit port so the harness sees a crash.
- **Allocator**: first-fit free list with coalescing over regions handed to it
  explicitly. A 4 MiB static arena is attached at boot; the PMM will attach the
  rest of RAM in phase 3.
- **Self tests**: twelve in-kernel checks (`minx.test=1`) plus a host-compiled
  allocator fuzz test (`make test-host`).

## Known bugs / gaps
- Interrupts do not reach the CPU (see the blocker section above).
- Only the boot CPU is running; `percpu_start_ap()` reports why and declines.
- The framebuffer resolution is whatever Limine picks (1280x800 under QEMU
  here); the `resolution:` option in `boot/limine.conf` is not honoured by the
  BIOS path, so the console adapts to whatever arrives.
- The initrd is unpacked by nothing yet: it is loaded and its size is reported,
  but phase 8 writes the ramfs unpacker.
- The TSC frequency could not be detected from CPUID under KVM here, so
  `delay_ms()` falls back to assuming 1 GHz. Delays are therefore a minimum,
  never shorter than requested. The PIT tick counter is the trustworthy uptime
  source; `uptime` reports both.
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
```

## Push status
`git push origin HEAD` fails in this environment with
`could not read Username for 'https://github.com'` — no credential helper and no
token available, and AGENTS.md forbids putting tokens in files or remotes.
Everything is committed locally in logical commits with the model co-author
trailer; push as soon as credentials exist. The earlier phase-one attempt hit
the same wall (see commit "docs: record phase one push limitation").