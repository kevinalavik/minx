# SPEC: Unix-like OS for amd64

## Goal
From a clean checkout, `make run` builds kernel, libc, userspace, initrd and a bootable image and
launches QEMU with a graphical window (`-smp 4` default, `SMP=N` override, serial also on stdio).
The system boots to an interactive shell on the **graphical framebuffer console** with all CPUs
online. `GUI=0` gives headless/serial-only operation.

## Hard constraints
1. x86-64 long mode only, higher-half kernel.
2. GNU make + standard host toolchain. Bootloader is your choice (Limine recommended: 64-bit
   entry, memory map, framebuffer, RSDP, modules for the initrd, SMP info). BIOS/ISO is the
   required default so `make run` works without OVMF; UEFI optional.
3. Third-party components are allowed and encouraged where they save effort (Limine, Flanterm,
   dlmalloc, uACPI, mlibc/musl/newlib, toybox/busybox/sbase, etc.). Research what exists first.
   Pin versions, track licenses in `docs/THIRD_PARTY.md`, keep thin wrappers.
4. Kernel core (scheduler, MM, VFS, SMP, syscalls) should be your own code unless you document
   why a third-party component is a better choice.
5. No stubs presented as features. The kernel never trusts userspace pointers.
6. Committed code is never removed (see AGENTS.md "Non-destructive rule").

## Kernel features

### Boot, CPU and console
- Own GDT/TSS (IST stacks for NMI/#DF), IDT with all exceptions, panic with register dump and
  stack trace on both framebuffer and serial.
- **Framebuffer console from Phase 1** (Limine framebuffer + Flanterm or own renderer + bitmap
  font), mirrored to COM1 serial. ANSI colors, cursor, scrolling, any resolution given by the
  bootloader.
- Keyboard (PS/2 scancode sets, shift/ctrl/caps, arrows) feeds the TTY; the shell runs on the
  graphical console. Optional: mouse, USB HID, virtio-input, scrollback (Shift+PgUp).
- ACPI (RSDP/XSDT/MADT) for CPU/LAPIC/IOAPIC discovery (own parser or uACPI); LAPIC + IOAPIC;
  legacy PIC masked. Calibrated LAPIC timer, monotonic clock (TSC/HPET), RTC wall clock.
- FPU/SSE state saved per task (FXSAVE/XSAVE).

### SMP
- Bring up all APs; per-CPU stacks, GDT/TSS, IDT, LAPIC timer, per-CPU data via GS (`swapgs`).
- Ticket/MCS spinlocks with IRQ-save variants, mutexes, semaphores, wait queues, atomics,
  barriers. Lock order documented in `docs/LOCKING.md`.
- IPIs: TLB shootdown, reschedule, panic/halt-all.
- Preemptive scheduler with per-CPU run queues, load balancing/stealing, priorities or
  MLFQ/CFS-like fairness, idle task per CPU. No big kernel lock in the final design.
- `smptest` proves more than one CPU runs user code concurrently.

### Memory
- PMM from the boot memory map (buddy or bitmap+free lists); reserve kernel, modules, ACPI.
- 4-level paging, higher-half direct map, kernel heap (own slab/kmalloc or dlmalloc-style),
  NX, SMEP/SMAP/WP where supported.
- Per-process address spaces (own PML4, shared kernel half), demand paging, brk/sbrk, anonymous
  mmap, stack growth, **copy-on-write fork**, page-fault handler distinguishing COW /
  demand-zero / stack growth / illegal access (SIGSEGV). `mmap/munmap/mprotect` (anonymous +
  private file-backed). Refcounted pages, correct cross-CPU TLB flushes. Graceful OOM.

### Processes and syscalls
- PID/PPID, process groups, sessions, controlling TTY, UID/GID with permission checks, kernel threads.
- `SYSCALL/SYSRET`. Minimum set (document in `docs/SYSCALLS.md`): fork, execve, exit, wait4,
  getpid, getppid, kill, sigaction, sigreturn, sigprocmask, pause, nanosleep, clock_gettime,
  brk, mmap, munmap, mprotect, open, close, read, write, lseek, pread/pwrite, stat/fstat/lstat,
  getdents, mkdir, rmdir, unlink, rename, link, symlink, readlink, chdir, getcwd, dup, dup2,
  pipe, fcntl, ioctl, chmod, chown, umask, utimes, truncate, mount, umount, uname, sched_yield,
  getuid/geteuid/setuid, setsid, setpgid, termios via ioctl. (If porting mlibc/musl, add what
  its sysdeps need.)
- ELF64 static loader (argv/envp/auxv per SysV ABI), `#!` support.
- POSIX signals (frames, SIGINT/SIGTSTP from TTY, SIGCHLD, SIGPIPE, SIGSEGV, uncatchable
  SIGKILL/SIGSTOP). Zombie reaping, orphans reparented to PID 1.

### VFS and filesystems
- Real VFS: superblocks, inodes, dentries + cache, file objects, mount table, path resolution
  (`.`/`..`, symlinks, mount crossing, permissions), per-process fd table/cwd/root.
- **initrd** (ustar or cpio newc) loaded as a boot module and unpacked into a **ramfs/tmpfs
  mounted as `/`**. Layout: `/bin /sbin /etc /dev /proc /tmp /home /root /usr`.
- Fully writable tmpfs (create/delete/rename/truncate/symlinks/hardlinks/perms/timestamps).
- devfs: `null zero full random urandom console tty tty0 ttyS0`.
- procfs: `cpuinfo meminfo uptime mounts <pid>/status self`.
- Pipes/FIFOs as VFS objects. Bonus: ext2/FAT32 on virtio-blk/AHCI.

### Devices and TTY
- TTY line discipline (canonical/raw, echo, ICANON/ISIG/ECHO, backspace, ^C ^D ^Z ^U ^W),
  job control, termios. Drivers: 16550 serial, PS/2 keyboard, PIT/LAPIC/HPET, RTC.
  Bonus: virtio-blk/net, USB.

## Userspace

### libc
Choose one and document it: **mlibc** (port a sysdeps layer for the kernel's syscalls), musl,
newlib, or an own libc. Must provide ISO C + POSIX essentials: stdio, stdlib/malloc, string,
unistd, fcntl, dirent, signal, setjmp, time, termios, sys/stat, sys/wait, glob/fnmatch/regex,
getopt. Userspace links statically with `-nostdlib` plus the chosen libc.

### init and shell
- `/sbin/init` (PID 1): mounts `/proc` and `/dev`, sets up the TTY, spawns `/bin/sh` (or login)
  on the console, reaps orphans, `poweroff`/`reboot` (ACPI / QEMU debug-exit / KBC).
- `/bin/sh`: quoting, `$VAR ${VAR} $? $$ $1...`, pipes, `< > >> 2> 2>&1`, `&& || ; &`, `( )`,
  `$( )`, globbing, PATH search, builtins (cd exit export unset set echo pwd read exec source/.
  alias type wait jobs fg bg true false test/[), `if/elif/else`, `for`, `while`, `case`,
  functions, shebang scripts. Bonus: line editing + history.

### Coreutils (own or third-party such as toybox/busybox/sbase/ubase, mixing allowed)
ls(-l -a -R -h -1) cat echo cp(-r) mv rm(-r -f) mkdir(-p) rmdir ln(-s) touch chmod chown pwd
head tail wc sort uniq cut tr tee grep(-i -v -n -r) sed(s p d -n) find(-name -type) xargs
basename dirname env printenv true false yes sleep date uname hostname id whoami ps kill free df
mount umount dmesg clear reset stty od/hexdump cmp diff du stat tar expr seq which wait su/login
more/less, and a **required text editor** (ed/vi-lite/nano-lite). Bonus: awk subset, top.
Own test programs are still required: forktest pipetest sigtest mmaptest cowtest smptest
stresstest, plus a usertests suite.

## Repository layout (adjust and document if needed)
```
Makefile README.md AGENTS.md CLAUDE.md knowledge.md .gitignore
docs/    DECISIONS.md STATUS.md LOCKING.md SYSCALLS.md MEMORY.md ARCH.md SPEC.md THIRD_PARTY.md
.githooks/ tools/ boot/ kernel/{arch/x86_64,mm,sched,fs,drivers,ipc,syscall,lib}/
libc/ userland/ initrd/ third_party/ tests/
```

## Makefile
- Targets: `all run debug test iso clean distclean help`. Parallel-safe (`-j`), incremental
  (`-MMD -MP`). `clean`/`distclean` only remove build output.
- `run`: `-smp $(SMP) -m 512M -serial stdio -vga std -no-reboot -device isa-debug-exit
  -accel kvm:tcg`, auto-detected display backend (gtk/sdl); `GUI=0` adds `-display none`.
- `debug`: QEMU `-s -S` plus `.gdbinit`.
- `test`: always headless, boots, runs the automated suite, parses PASS/FAIL markers, verifies
  the framebuffer (QEMU monitor `screendump` or serial markers), enforces timeouts, exits
  non-zero on failure.
- `-Wall -Wextra`; warning-free for own code in the final state.

## Phases (verify each by running before moving on; commit, try pushing, continue)
1. Toolchain & boot: Makefile, bootloader, **framebuffer console + serial "hello"**, panic
   handler visible on both.
2. CPU core: GDT/TSS/IDT, exceptions, ACPI/MADT, LAPIC/IOAPIC, timer, **PS/2 keyboard typing
   into the graphical console**.
3. Memory: PMM, VMM, heap, direct map, page-fault handler; in-kernel allocator stress test.
4. SMP: AP bring-up, per-CPU data, spinlocks, IPIs; every CPU prints "CPU n online"; contention test.
5. Scheduler & kernel threads: preemptive per-CPU scheduling, wait queues, sleep.
6. Userspace entry: syscalls, ELF loader, ring-3 `hello`.
7. Processes: fork (COW), exec, wait, exit, signals, pipes (forktest, cowtest, sigtest).
8. VFS + ramfs + initrd + devfs; `/init` runs from initrd.
9. TTY + libc + sh: interactive shell on the graphical console, pipelines, redirection, scripts.
10. Coreutils + procfs: each tool has at least one automated test.
11. Hardening: `stresstest` (forks, pipes, mmap/munmap, signals) on 1/2/4/8 CPUs, no panics,
    deadlocks, or leaks (PMM free-page accounting before/after).
12. Polish: docs, bonus features, warning-free build, README additions (first section only).

## Definition of Done
1. `make run` on a clean clone opens a window with the graphical console, boots to a shell with
   N CPUs online (boot log + `/proc/cpuinfo`).
2. These work in the shell: `ls -l /`, `cat /proc/meminfo`, `echo hi | tr a-z A-Z`,
   `for i in 1 2 3; do echo $i; done`, `sleep 1 &`, `grep -r root /etc | sort | uniq -c | wc -l`,
   create/edit/delete files in tmpfs, run a shebang script.
3. fork+COW, exec, pipes, signals, job control (^C ^Z fg bg), and mmap demonstrably work.
4. `smptest` shows CPU-bound processes running in parallel on different CPUs.
5. `make test` passes; `stresstest` runs 10+ minutes at SMP=4 without panic or hang.
6. Colors and scrolling work on the graphical console.
7. Warning-free build, documented, committed in logical commits and pushed to GitHub.
8. `docs/THIRD_PARTY.md` lists every external component with version and license.
9. No committed code has been removed, and every commit carries the model co-author trailer.
10. `docs/STATUS.md` honestly lists known gaps; README only extended under its first section.
