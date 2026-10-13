# THIRD PARTY COMPONENTS
| Name | Version/commit | URL | License | Used for | Why chosen | How included (vendored/fetched) |
|------|----------------|-----|---------|----------|------------|----------------------------------|
| Limine | v12.9.1 (release `v12.9.1`) | https://github.com/limine-bootloader/limine | MIT (bootloader payload); source tarball `COPYING` | Bootloader: 64-bit entry, memory map, framebuffer, RSDP, SMP per-CPU entry points, modules (the initrd) | A complete long-mode bootloader with the exact handover data SPEC.md needs, so no firmware work has to be written from scratch. The release ships the `limine` host tool's source, so `bios-install` is compiled locally. | Fetched by the Makefile with SHA-256 verification: `limine-binary.tar.xz` (sha256 `ce972a05…17b8`) supplies the BIOS/UEFI payloads and the host tool source; `limine-12.9.1.tar.bz2` (sha256 `e1c78e71…2ab`) supplies `limine-protocol/include/limine.h`, which is lifted into `build/include/`. Downloads land in the gitignored `third_party/_fetched/`. Licence text is copied into the ISO as `limine-LICENSE`. |
| kbd (Linux keyboard utilities) console font | `lat9-16.psf` from kbd 2.9.0 | https://mirrors.edge.kernel.org/pub/linux/utils/kbd/ | GPL-2.0-or-later (the kbd sources); the font data is the classic IBM VGA/CP437 8x16 glyph set, also distributed under the same terms by viler-int10h's vga-text-mode-fonts | Bitmap font for the graphical console | A complete, well-tested CP437 8x16 font rather than a hand-written one. Copyleft, but only the font *data* is taken, and the whole project is public domain (UNLICENSE) — noted here as required. | Fetched by the Makefile with SHA-256 verification (`kbd-2.9.0.tar.xz`, sha256 `fb3197f1…2ed`); only the single 4 KiB `lat9-16.psf` file is extracted, then converted to a C array at build time by `tools/psf2c.py`. |
| Flanterm | v3.0.2 (release `v3.0.2`) | https://github.com/Mintsuki/Flanterm | BSD-2-Clause | Framebuffer terminal emulator: everything the graphical console draws | `docs/SPEC.md` names it as the intended choice and it is written for exactly this job: freestanding C99, no libc beyond `memcpy`/`memset`, and a backend written against Limine's framebuffer, which is what this kernel is handed. It brings a scrollback buffer, an alternate screen and the rest of the CSI set, which is what a line editor, `less` and `vi` need and what a first-party console grew only in pieces. | Fetched by the Makefile with SHA-256 verification: `flanterm-3.0.2.tar.gz` (sha256 `90484ec7…77b1`); `src/flanterm.c` and `src/flanterm_backends/fb.c` are compiled into the kernel with the kernel's own flags and nothing but the kernel's headers. Downloads land in the gitignored `third_party/_fetched/`. The library's `LICENSE` is kept intact in the fetched tree. |

## Notes
- The kernel-facing Limine header (`limine.h`) is fetched from the *source*
  tarball because the binary release does not contain it. Only the header is
  used; none of Limine's own code is compiled into the kernel.
- The framebuffer terminal was first-party (see docs/DECISIONS.md) and is now
  flanterm. The first-party console is still in the tree and still builds —
  `make TERMINAL=own` selects it — because a console that is replaced on the
  strength of a claim about the replacement is not a decision, and because its
  bugs are the record of what that layer is like. `kernel/drivers/term_flanterm.c`
  and `kernel/drivers/console.c` implement the same `console.h` and nothing else in
  the tree knows which is in use.
- flanterm's font argument is a raw VGA-style bitmap — one byte per row, eight
  pixels wide — which is exactly the layout `tools/psf2c.py` already emits for
  `g_font8x16`. The generated font is passed straight through, so no third party's
  font bytes are in the kernel and there is no second font licence to reason about.
- flanterm queues dirty cells and draws them in `flanterm_flush()`; it has no
  timer of its own. `term_flanterm.c` flushes after every write. Without that,
  nothing is drawn at all after the first flush — see the comment there.
- Host build tools (clang, ld.lld, nasm, xorriso, qemu) are prerequisites of the
  host and are never downloaded by the build.