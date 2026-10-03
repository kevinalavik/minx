########################################################################
# minx - a Unix-like OS for x86-64
#
# Quick start:
#   make            build kernel, libc, userspace and the bootable ISO
#   make run        boot it in QEMU with a graphical window
#   GUI=0 make run  headless: serial console only
#   make test       headless automated test run, non-zero exit on failure
#   make debug      boot paused under GDB
#   make help       list every target and variable
#
# Nothing below needs manual steps: third-party assets are fetched and
# checksum-verified automatically (see docs/THIRD_PARTY.md).
########################################################################

.SUFFIXES:
.DEFAULT_GOAL := all

########################################################################
# Configuration
########################################################################

SMP      ?= 4                  # number of CPUs handed to QEMU
MEM      ?= 512M               # guest RAM
GUI      ?= 1                  # 0 => headless (-display none)
QEMU     ?= qemu-system-x86_64
PYTHON   ?= python3
SERIAL   ?= stdio
DEBUG_LOG?= build/qemu.log

KERNEL_BASE ?= 0xFFFFFFFF80000000

########################################################################
# Host toolchain
#
# Prefer clang/LLVM as AGENTS.md asks, but fall back to GCC when clang is
# unusable (e.g. a host where llvm-libs does not match the clang package).
# Nothing is ever downloaded here -- the host toolchain is a prerequisite.
########################################################################

CC_candidates := clang gcc cc
CC := $(shell for c in $(CC_candidates); do \
            if command -v $$c >/dev/null 2>&1 && $$c --version >/dev/null 2>&1; \
            then echo $$c; break; fi; \
        done)
ifeq ($(strip $(CC)),)
$(error no working C compiler found; install clang or gcc)
endif

# Matched assembler/linker: lld with clang, ld.bfd/gold with gcc.
ifeq ($(notdir $(CC)),clang)
LD := ld.lld
# Freestanding headers (stdint.h, stddef.h, stdarg.h, ...) that ship with
# the compiler itself rather than with libc.
FREESTD_INC := $(shell $(CC) -print-resource-dir)/include
else
LD := $(CC)
FREESTD_INC := $(shell $(CC) -print-file-name=include)
endif

ifeq ($(strip $(LD)),)
LD := ld
endif

AS_candidates := nasm
AS := $(shell for a in $(AS_candidates); do \
            if command -v $$a >/dev/null 2>&1; then echo $$a; break; fi; \
        done)

XORRISO := $(shell command -v xorriso 2>/dev/null)
GDB     := $(shell command -v gdb 2>/dev/null)

########################################################################
# Pinned third-party assets (see docs/THIRD_PARTY.md)
########################################################################

TP          := third_party/_fetched
LIMINE_VER  := 12.9.1
LIMINE_URL  := https://github.com/limine-bootloader/limine/releases/download/v$(LIMINE_VER)/limine-binary.tar.xz
LIMINE_SHA  := ce972a05e9d1973dc9b725f9130bd15af01add0eb3a7f90c118ac5cfe21c17b8
LIMINE_TAR  := $(TP)/limine-binary.tar.xz

# The kernel-facing protocol header is not in the binary release, so the source
# tarball is fetched too (same version) and limine.h is lifted out of it.
LIMINE_SRC_URL := https://github.com/limine-bootloader/limine/releases/download/v$(LIMINE_VER)/limine-$(LIMINE_VER).tar.bz2
LIMINE_SRC_SHA := e1c78e7148bd282a948fb408e13737c8f4a63ff095bdb291a12e5b59827052ab
LIMINE_SRC_TAR := $(TP)/limine-$(LIMINE_VER).tar.bz2

KBD_VER     := 2.9.0
KBD_URL     := https://mirrors.edge.kernel.org/pub/linux/utils/kbd/kbd-$(KBD_VER).tar.xz
KBD_SHA     := fb3197f17a99eb44d22a3a1a71f755f9622dd963e66acfdea1a45120951b02ed
KBD_TAR     := $(TP)/kbd-$(KBD_VER).tar.xz
FONT_PSF    := $(TP)/font-lat9-16.psf

########################################################################
# Paths
########################################################################

BUILD      := build
KERNEL_DIR := kernel
LIBC_DIR   := libc
USER_DIR   := userland
TESTS_DIR  := tests
INITRD_SRC := initrd
ISO_DIR    := $(BUILD)/isodir
LIMINE_DIR := $(TP)/limine-binary

# Generated headers/assets live under build/ but must be known before the flag
# definitions below, which reference them with an immediate assignment.
GEN_INC     := $(BUILD)/include
FONT_C      := $(BUILD)/font_lat9_16.c
FONT_OBJ    := $(BUILD)/k/font.o
LIMINE_H    := $(GEN_INC)/limine.h

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
INITRD     := $(BUILD)/initrd.cpio
ISO        := $(BUILD)/minx.iso
ISO_TEST   := $(BUILD)/minx-test.iso
LIMINE_TOOL:= $(TP)/limine-binary/limine

########################################################################
# Flags
########################################################################

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-address-of-packed-member
OPT      ?= -O2 -g
COMMON   := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
            -fno-asynchronous-unwind-tables -mno-red-zone -mcmodel=kernel \
            -mno-mmx -mno-sse -mno-sse2 -msoft-float -mno-80387 \
            -nostdinc -isystem $(FREESTD_INC) -m64 -MMD -MP \
            -I$(KERNEL_DIR)/include -I$(GEN_INC) -D__MINX__=1
KERNEL_CFLAGS := $(COMMON) $(OPT) $(WARN) -std=gnu11 -D__KERNEL__=1 \
            -fno-stack-protector -fno-builtin
KERNEL_ASFLAGS := -ffreestanding -nostdinc -isystem $(FREESTD_INC) -MMD -MP \
            -I$(KERNEL_DIR)/include -I$(GEN_INC) -D__KERNEL__=1 -m64
KERNEL_LDFLAGS := -n -T $(KERNEL_DIR)/boot/linker.ld -z max-page-size=4096 \
            -static --no-dynamic-linker -z noexecstack --build-id=none

# Userspace and libc: normal hosted-less static build.
USER_CFLAGS := -std=gnu11 -ffreestanding -nostdlib -nostdinc \
            -isystem $(FREESTD_INC) -fno-stack-protector -fno-pic -fno-pie \
            -fno-asynchronous-unwind-tables -mno-red-zone -mno-mmx -mno-sse \
            -mno-sse2 -m64 -MMD -MP $(OPT) $(WARN) \
            -I$(LIBC_DIR)/include -I$(LIBC_DIR)/src -D__MINX__=1
USER_LDFLAGS := -static -nostdlib -no-pie -z noexecstack --build-id=none \
            -L$(BUILD)/lib -lc

########################################################################
# Sources
########################################################################

KERNEL_C := $(shell find $(KERNEL_DIR) -name '*.c' 2>/dev/null | sort)
KERNEL_S := $(shell find $(KERNEL_DIR) -name '*.S' 2>/dev/null | sort)

KERNEL_OBJ := $(KERNEL_C:%.c=$(BUILD)/k/%.o) $(KERNEL_S:%.S=$(BUILD)/k/%.o) \
             $(FONT_OBJ)

LIBC_C   := $(shell find $(LIBC_DIR)/src -name '*.c' 2>/dev/null | sort)
LIBC_ASM := $(shell find $(LIBC_DIR)/src -name '*.S' 2>/dev/null | sort)
LIBC_OBJ := $(LIBC_C:%.c=$(BUILD)/u/%.o) $(LIBC_ASM:%.S=$(BUILD)/u/%.o)
LIBC     := $(BUILD)/lib/libc.a

USER_BIN := $(foreach prog,$(shell ls $(USER_DIR) 2>/dev/null | grep -v -e '\.' -e Makefile), \
                        $(BUILD)/bin/$(prog))

TEST_BIN := $(foreach prog,$(shell ls $(TESTS_DIR) 2>/dev/null | grep -v -e '\.' -e Makefile), \
                        $(BUILD)/bin/$(prog))

ALL_BIN := $(USER_BIN) $(TEST_BIN)

# Pull in the generated header dependencies so incremental builds actually
# notice header changes.
-include $(KERNEL_OBJ:.o=.d) $(LIBC_OBJ:.o=.d)

# Directory tree shipped in the initrd, built into build/initrd-root.
INITRD_ROOT := $(BUILD)/initrd-root

.PHONY: all kernel userspace iso iso-test run test test-host debug clean \
        distclean help fetch font tools-check

########################################################################
# Rules
########################################################################

all: iso

help:
	@echo "minx - targets:"
	@echo "  all        build everything (kernel, libc, userspace, initrd, ISO)"
	@echo "  kernel     build the kernel ELF and raw binary"
	@echo "  userspace  build libc, userland programs and tests"
	@echo "  iso        build $(ISO)"
	@echo "  run        boot in QEMU (GUI=$(GUI) SMP=$(SMP) MEM=$(MEM))"
	@echo "  test       headless automated test run; non-zero exit on failure"
	@echo "  test-host  host-compiled unit tests (allocator, formatting)"
	@echo "  debug      boot paused for GDB (SMP=$(SMP) MEM=$(MEM))"
	@echo "  clean      remove build output"
	@echo "  distclean  clean + remove fetched third-party assets"
	@echo "  fetch      download + verify third-party assets only"
	@echo
	@echo "variables: SMP=$(SMP) MEM=$(MEM) GUI=$(GUI) CC=$(CC) LD=$(LD)"

# ---------------------------------------------------------------- assets ---

fetch: $(LIMINE_TAR) $(LIMINE_SRC_TAR) $(FONT_PSF)

$(LIMINE_TAR):
	@sh tools/fetch.sh limine "$(LIMINE_URL)" "$(LIMINE_SHA)" "$@"
	@mkdir -p $(LIMINE_DIR)
	@tar -xJf "$@" -C $(TP)
	@echo "unpacked limine -> $(LIMINE_DIR)"

$(LIMINE_SRC_TAR):
	@sh tools/fetch.sh limine-source "$(LIMINE_SRC_URL)" "$(LIMINE_SRC_SHA)" "$@"

$(LIMINE_H): $(LIMINE_SRC_TAR)
	@mkdir -p $(GEN_INC)
	@tar -xjf "$<" -C $(TP) --strip-components=3 \
	    limine-$(LIMINE_VER)/limine-protocol/include/limine.h
	@mv -f $(TP)/limine.h $@

$(KBD_TAR):
	@sh tools/fetch.sh kbd "$(KBD_URL)" "$(KBD_SHA)" "$@"

$(FONT_PSF): $(KBD_TAR)
	@tar -xf "$<" -C $(TP) --strip-components=3 \
	    kbd-$(KBD_VER)/data/consolefonts/lat9-16.psf
	@mv -f $(TP)/lat9-16.psf $@
	@echo "extracted console font -> $@"

font: $(FONT_C)

$(FONT_C): $(FONT_PSF) tools/psf2c.py
	@mkdir -p $(dir $@)
	@$(PYTHON) tools/psf2c.py $(FONT_PSF) g_font8x16 $@

$(FONT_OBJ): $(FONT_C) | $(LIMINE_H)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# ---------------------------------------------------------------- kernel ---

kernel: $(KERNEL_ELF) $(KERNEL_BIN)

$(BUILD)/k/%.o: %.c | $(LIMINE_H)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.S | $(LIMINE_H)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(KERNEL_DIR)/boot/linker.ld $(LIMINE_H)
	@mkdir -p $(dir $@)
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJ) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	@objcopy -O binary $< $@

# ------------------------------------------------------------------ libc ---

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/u/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIBC): $(LIBC_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(LIBC_OBJ)

userspace: $(LIBC) $(ALL_BIN)

$(BUILD)/bin/%: $(USER_DIR)/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) $(USER_CFLAGS) $< -o $@

$(BUILD)/bin/%: $(TESTS_DIR)/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) $(USER_CFLAGS) $< -o $@

# ---------------------------------------------------------------- initrd ---

$(INITRD_ROOT)/.stamp: $(ALL_BIN) $(INITRD_SRC)/tree.stamp
	@mkdir -p $(INITRD_ROOT)
	@rm -rf $(INITRD_ROOT)
	@mkdir -p $(INITRD_ROOT)/bin $(INITRD_ROOT)/sbin $(INITRD_ROOT)/etc \
	          $(INITRD_ROOT)/dev $(INITRD_ROOT)/proc $(INITRD_ROOT)/tmp \
	          $(INITRD_ROOT)/home $(INITRD_ROOT)/root $(INITRD_ROOT)/usr/bin \
	          $(INITRD_ROOT)/usr/lib $(INITRD_ROOT)/usr/share
	@for b in $(ALL_BIN); do \
	    install -m 755 $$b $(INITRD_ROOT)/bin/; \
	done
	@for b in $(INITRD_ROOT)/bin/init $(INITRD_ROOT)/bin/sh; do \
	    test -f $$b || continue; \
	done
	@if [ -d $(INITRD_SRC)/etc ]; then cp -a $(INITRD_SRC)/etc/. $(INITRD_ROOT)/etc/; fi
	@if [ -d $(INITRD_SRC)/home ]; then cp -a $(INITRD_SRC)/home/. $(INITRD_ROOT)/home/; fi
	@if [ -d $(INITRD_SRC)/root ]; then cp -a $(INITRD_SRC)/root/. $(INITRD_ROOT)/root/; fi
	@if [ -d $(INITRD_SRC)/usr ]; then cp -a $(INITRD_SRC)/usr/. $(INITRD_ROOT)/usr/; fi
	@touch $@

$(INITRD_SRC)/tree.stamp:
	@touch $@

$(INITRD): $(INITRD_ROOT)/.stamp
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkinitrd.py $@ $(INITRD_ROOT)

# ------------------------------------------------------------------- iso ---

iso: $(ISO)

# The Limine host tool (bios-install) ships as C source in the release tarball
# and is built with the host compiler; nothing extra is downloaded.
$(LIMINE_TOOL): $(LIMINE_TAR)
	$(MAKE) --no-print-directory -C $(LIMINE_DIR) CC="cc"

# $(1) = iso path, $(2) = config file to install as /boot/limine.conf
define build_iso
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot $(ISO_DIR)/EFI/BOOT
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/minx.bin
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/minx.elf
	@cp $(INITRD) $(ISO_DIR)/boot/minx-initrd.cpio
	@cp $(2) $(ISO_DIR)/boot/limine.conf
	@cp $(2) $(ISO_DIR)/boot/limine-orig.conf
	@cp $(LIMINE_DIR)/limine-bios.sys $(ISO_DIR)/limine-bios.sys
	@cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_DIR)/boot/limine-bios-cd.bin
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_DIR)/boot/limine-uefi-cd.bin
	@cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI
	@cp $(LIMINE_DIR)/LICENSE $(ISO_DIR)/limine-LICENSE
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISO_DIR) -o $@
	$(LIMINE_TOOL) bios-install $@
	@echo "built $@"
endef

$(ISO): $(KERNEL_BIN) $(INITRD) $(KERNEL_ELF) boot/limine.conf $(LIMINE_TOOL)
	@test -n "$(XORRISO)" || { echo "xorriso is required to build the ISO" >&2; exit 1; }
	$(call build_iso,$@,boot/limine.conf)

$(ISO_TEST): $(KERNEL_BIN) $(INITRD) $(KERNEL_ELF) boot/limine-test.conf $(LIMINE_TOOL)
	@test -n "$(XORRISO)" || { echo "xorriso is required to build the ISO" >&2; exit 1; }
	$(call build_iso,$@,boot/limine-test.conf)

iso-test: $(ISO_TEST)

# ------------------------------------------------------------------- run ---

# Pick the first display backend this QEMU was built with; fall back to a
# headless framebuffer-only run when there is none.
DISPLAY_BACKEND := $(shell for d in gtk sdl cocoa; do \
                      if $(QEMU) -display help 2>/dev/null | grep -qw $$d; \
                      then echo $$d; break; fi; \
                  done)
ifeq ($(filter 0,$(GUI)),0)
DISPLAY_ARGS := -display none
else
DISPLAY_ARGS := $(if $(DISPLAY_BACKEND),-display $(DISPLAY_BACKEND),-display none)
endif

ifeq ($(shell test -r /dev/kvm && echo yes),yes)
ACCEL := -accel kvm
else
ACCEL := -accel tcg,thread=multi
endif

# The ISO is attached as a plain IDE CD-ROM; `if=pci` is not a valid bus.
QEMU_COMMON := -m $(MEM) -smp $(SMP) \
                -no-reboot \
                -boot d -drive file=$(ISO),format=raw,if=none,id=minxcd \
                -device ide-cd,drive=minxcd

run: iso
	$(QEMU) $(QEMU_COMMON) -serial $(SERIAL) -vga std $(DISPLAY_ARGS) $(ACCEL)

debug: iso
	@test -n "$(GDB)" || { echo "gdb not found" >&2; exit 1; }
	@printf 'set architecture i386:x86-64\ntarget remote localhost:1234\n' > $(BUILD)/.gdbinit
	$(QEMU) $(QEMU_COMMON) -device isa-debug-exit,iobase=0xf4,iosize=0x04  -serial $(SERIAL) -vga std $(DISPLAY_ARGS) $(ACCEL) \
	    -s -S -monitor none

test: $(ISO_TEST)
	SMP=$(SMP) MEM=$(MEM) QEMU="$(QEMU)" BUILD="$(BUILD)" \
	    ISO="$(ISO_TEST)" sh tools/run-tests.sh

# Host-compiled unit tests for the parts of the kernel that do not need a CPU
# to exercise.  Fast, and they catch allocator and formatting bugs without a
# 30-second boot cycle.
test-host: $(BUILD)/kmalloc-host-test
	$(BUILD)/kmalloc-host-test

$(BUILD)/kmalloc-host-test: tools/kmalloc-host-test.c $(KERNEL_DIR)/mm/kmalloc.c \
                            $(KERNEL_DIR)/lib/string.c $(KERNEL_DIR)/lib/kprintf.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O1 -g -I$(KERNEL_DIR)/include $^ -o $@

# ----------------------------------------------------------------- clean ---

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf third_party/_fetched