SHELL := /bin/bash
.DEFAULT_GOAL := help

CC ?= gcc
LD ?= ld
QEMU ?= qemu-system-x86_64
XORRISO ?= xorriso
SMP ?= 4
BUILD := build
KERNEL := $(BUILD)/minx.elf
ISO := $(BUILD)/minx.iso
TEST_ISO := $(BUILD)/minx-test.iso
LIMINE_VERSION := 12.9.1
LIMINE_SHA256 := ce972a05e9d1973dc9b725f9130bd15af01add0eb3a7f90c118ac5cfe21c17b8
LIMINE_ARCHIVE := $(BUILD)/limine-binary.tar.xz
LIMINE_DIR := $(BUILD)/limine
KERNEL_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
	-fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-m64 -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -MMD -MP

QEMU_DISPLAY := -display none
ifeq ($(GUI),1)
QEMU_DISPLAY := -display default
endif
QEMU_BASE := -machine q35 -cpu qemu64 -smp $(SMP) -m 512M -serial stdio $(QEMU_DISPLAY) \
	-no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all run debug test iso clean distclean help
all: $(ISO)

$(BUILD):
	mkdir -p $@

$(BUILD)/kernel.o: kernel/boot/kernel.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL): $(BUILD)/kernel.o kernel/boot/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/boot/linker.ld -o $@ $(BUILD)/kernel.o

$(LIMINE_ARCHIVE): | $(BUILD)
	curl --fail --location --silent --show-error \
		https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.xz \
		-o $@
	echo "$(LIMINE_SHA256)  $@" | sha256sum --check

$(LIMINE_DIR)/.stamp: $(LIMINE_ARCHIVE)
	mkdir -p $(LIMINE_DIR)
	tar -xJf $(LIMINE_ARCHIVE) --strip-components=1 -C $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)
	test -f $(LIMINE_DIR)/limine-bios.sys -a -f $(LIMINE_DIR)/limine-bios-cd.bin -a -x $(LIMINE_DIR)/limine
	touch $@

$(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine: $(LIMINE_DIR)/.stamp
	test -f $@ -o -x $@

$(BUILD)/iso/.stamp: $(KERNEL) $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin boot/limine.conf
	mkdir -p $(BUILD)/iso/boot
	cp $(KERNEL) $(BUILD)/iso/boot/minx.elf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(BUILD)/iso/boot/
	cp boot/limine.conf $(BUILD)/iso/limine.conf
	touch $@

$(BUILD)/test-iso/.stamp: $(KERNEL) $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin boot/limine-test.conf
	mkdir -p $(BUILD)/test-iso/boot
	cp $(KERNEL) $(BUILD)/test-iso/boot/minx.elf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(BUILD)/test-iso/boot/
	cp boot/limine-test.conf $(BUILD)/test-iso/limine.conf
	touch $@

$(ISO): $(BUILD)/iso/.stamp
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table $(BUILD)/iso -o $@
	$(LIMINE_DIR)/limine bios-install --force $@

$(TEST_ISO): $(BUILD)/test-iso/.stamp
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table $(BUILD)/test-iso -o $@
	$(LIMINE_DIR)/limine bios-install --force $@

iso: $(ISO)

run: $(ISO)
	$(QEMU) $(QEMU_BASE) $(if $(wildcard /dev/kvm),-enable-kvm,) -cdrom $(ISO)

debug: $(ISO)
	$(QEMU) $(QEMU_BASE) $(if $(wildcard /dev/kvm),-enable-kvm,) -cdrom $(ISO) -s -S

test: $(TEST_ISO)
	@set +e; timeout 5s $(QEMU) $(QEMU_BASE) -accel tcg -cdrom $(TEST_ISO) >$(BUILD)/qemu-test.log 2>&1; status=$$?; set -e; \
	cat $(BUILD)/qemu-test.log; \
	grep -q 'MINX: phase 1 boot OK' $(BUILD)/qemu-test.log; \
	grep -q 'MINX: self-test PASS' $(BUILD)/qemu-test.log; \
	test $$status -eq 33

clean:
	rm -rf $(BUILD)/iso $(BUILD)/test-iso $(KERNEL) $(BUILD)/kernel.o $(BUILD)/kernel.d $(ISO) $(TEST_ISO) $(BUILD)/qemu-test.log

distclean: clean
	rm -rf $(LIMINE_DIR) $(LIMINE_ARCHIVE)

help:
	@printf '%s\n' 'Targets: all run debug test iso clean distclean help' \
		'Variables: SMP=N (default 4), GUI=1 opens a graphical QEMU window'

-include $(BUILD)/kernel.d
