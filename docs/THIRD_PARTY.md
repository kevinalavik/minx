# THIRD PARTY COMPONENTS
| Name | Version/commit | URL | License | Used for | Why chosen | How included (vendored/fetched) |
|------|----------------|-----|---------|----------|------------|----------------------------------|
| Limine | v12.9.1 (release `v12.9.1`) | https://github.com/limine-bootloader/limine | MIT (bootloader payload); source tarball `COPYING` | Bootloader: 64-bit entry, memory map, framebuffer, RSDP, SMP per-CPU entry points, modules (the initrd) | A complete long-mode bootloader with the exact handover data SPEC.md needs, so no firmware work has to be written from scratch. The release ships the `limine` host tool's source, so `bios-install` is compiled locally. | Fetched by the Makefile with SHA-256 verification: `limine-binary.tar.xz` (sha256 `ce972a05…17b8`) supplies the BIOS/UEFI payloads and the host tool source; `limine-12.9.1.tar.bz2` (sha256 `e1c78e71…2ab`) supplies `limine-protocol/include/limine.h`, which is lifted into `build/include/`. Downloads land in the gitignored `third_party/_fetched/`. Licence text is copied into the ISO as `limine-LICENSE`. |
| kbd (Linux keyboard utilities) console font | `lat9-16.psf` from kbd 2.9.0 | https://mirrors.edge.kernel.org/pub/linux/utils/kbd/ | GPL-2.0-or-later (the kbd sources); the font data is the classic IBM VGA/CP437 8x16 glyph set, also distributed under the same terms by viler-int10h's vga-text-mode-fonts | Bitmap font for the graphical console | A complete, well-tested CP437 8x16 font rather than a hand-written one. Copyleft, but only the font *data* is taken, and the whole project is public domain (UNLICENSE) — noted here as required. | Fetched by the Makefile with SHA-256 verification (`kbd-2.9.0.tar.xz`, sha256 `fb3197f1…2ed`); only the single 4 KiB `lat9-16.psf` file is extracted, then converted to a C array at build time by `tools/psf2c.py`. |

## Notes
- The kernel-facing Limine header (`limine.h`) is fetched from the *source*
  tarball because the binary release does not contain it. Only the header is
  used; none of Limine's own code is compiled into the kernel.
- The framebuffer terminal is first-party (see docs/DECISIONS.md), so there is
  no terminal-emulator dependency.
- Host build tools (clang, ld.lld, nasm, xorriso, qemu) are prerequisites of the
  host and are never downloaded by the build.