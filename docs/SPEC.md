# SPEC — Unix-like OS for amd64

## Goal
From a clean checkout, `make run` builds kernel, libc, userspace, initrd and a bootable image and
launches QEMU (`-smp 4` default, `SMP=N` override). The system boots to an interactive shell on
serial (and framebuffer if implemented) with all CPUs online.

## Hard constraints
1. x86-64 long mode only, higher-half kernel.
2. GNU make + standard host toolchain. Bootloader is your choice (Limine recommended: 64-bit
   entry, memory map, framebuffer, RSDP, modules for initrd, SMP info). BIOS/ISO is the required
   default so `make run` works without OVMF; UEFI optional.
3. All code is yours except the bootloader. No code copied from Linux/BSD/musl/busybox.
4. C (C11/GNU) + assembly. No stubs presented as features.
5. The kernel never trusts userspace pointers.

## Kernel features
### Boot & CPU
- Own GDT/TSS (IST stacks for NMI/#DF), IDT with all exceptions, panic with register dump and
  stack trace over serial.
- COM1 serial console; optional framebuffer text console.
- ACPI (RSDP/XSDT/MADT) for CPU/LAPIC/IOAPIC discovery; LAPIC + IOAPIC; legacy PIC masked.
  Calibrated LAPIC timer, monotonic clock (TSC/HPET), RTC wall clock.
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
- 4-level paging, higher-half direct map, kernel heap (slab/kmalloc), NX, SMEP/SMAP/WP.
- Per-process address spaces, demand paging, brk/sbrk, anonymous mmap, stack growth,
  **copy-on-write fork**, page-fault handler distinguishing COW / demand-zero / stack growth /
  illegal access (SIGSEGV). `mmap/munmap/mprotect` (anonymous + private file-backed).
  Refcounted pages, correct cross-CPU TLB flushes. Graceful OOM.

### Processes & syscalls
- PID/PPID, process groups, sessions, controlling TTY, UID/GID with permission checks, kernel threads.
- `SYSCALL/SYSRET`. Minimum set (document in `docs/SYSCALLS.md`): fork, execve, exit, wait4,
  getpid, getppid, kill, sigaction, sigreturn, sigprocmask, pause, nanosleep, clock_gettime,
  brk, mmap, munmap, mprotect, open, close, read, write, lseek, pread/pwrite, stat/fstat/lstat,
  getdents, mkdir, rmdir, unlink, rename, link, symlink, readlink, chdir, getcwd, dup, dup2,
  pipe, fcntl, ioctl, chmod, chown, umask, utimes, truncate, mount, umount, uname, sched_yield,
  getuid/geteuid/setuid, setsid, setpgid, termios via ioctl.
- ELF64 static loader (argv/envp/auxv per SysV ABI), `#!` support.
- POSIX signals (frames, SIGINT/SIGTSTP from TTY, SIGCHLD, SIGPIPE, SIGSEGV, uncatchable
  SIGKILL/SIGSTOP). Zombie reaping, orphans reparented to PID 1.

### VFS & filesystems
- Real VFS: superblocks, inodes, dentries + cache, file objects, mount table, path resolution
  (`.`/`..`, symlinks, mount crossing, permissions), per-process fd table/cwd/root.
- **initrd** (ustar or cpio newc) loaded as a boot module and unpacked into a **ramfs/tmpfs
  mounted as `/`**. Layout: `/bin /sbin /etc /dev /proc /tmp /home /root /usr`.
- Fully writable tmpfs (create/delete/rename/truncate/symlinks/hardlinks/perms/timestamps).
- devfs: `null zero full random urandom console tty tty0 ttyS0`.
- procfs: `cpuinfo meminfo uptime mounts <pid>/status self`.
- Pipes/FIFOs as VFS objects. Optional bonus: ext2/FAT32 on virtio-blk/AHCI.

### Devices & TTY
- TTY line discipline (canonical/raw, echo, ICANON/ISIG/ECHO, backspace, ^C ^D ^Z ^U ^W),
  job control, termios. Drivers: 16550 serial, PS/2 keyboard, PIT/LAPIC/HPET, RTC.
  Bonus: framebuffer console, virtio-blk/net.

## Userspace
### libc (own, static)
crt0/crti/crtn, syscall wrappers, errno, stdio (FILE*, buffering, printf family, scanf basics),
stdlib (malloc family on brk/mmap, qsort, strtol family, getenv/setenv, atexit, abort, exit),
string, ctype, unistd, fcntl, dirent, signal, setjmp/longjmp, time, termios, sys/stat, sys/wait,
glob/fnmatch (or basic regex), getopt. Userspace links with `-nostdlib -static`.

### init and shell
- `/sbin/init` (PID 1): mounts `/proc` and `/dev`, sets up TTY, spawns `/bin/sh` (or login) on
  the console, reaps orphans, `poweroff`/`reboot` (ACPI / QEMU debug-exit / KBC).
- `/bin/sh`: quoting, `$VAR ${VAR} $? $$ $1…`, pipes, `< > >> 2> 2>&1`, `&& || ; &`, `( )`,
  `$( )`, globbing, PATH search, builtins (cd exit export unset set echo pwd read exec source/. alias
  type wait jobs fg bg true false test/[), `if/elif/else`, `for`, `while`, `case`, functions,
  shebang scripts. Bonus: line editing + history.

### Coreutils (own implementations, common flags)
ls(-l -a -R -h -1) cat echo cp(-r) mv rm(-r -f) mkdir(-p) rmdir ln(-s) touch chmod chown pwd
head tail wc sort uniq cut tr tee grep(-i -v -n -r) sed(s p d -n) find(-name -type) xargs
basename dirname env printenv true false yes sleep date uname hostname id whoami ps kill free df
mount umount dmesg clear reset stty od/hexdump cmp diff du stat tar expr seq which wait su/login
more/less, and a **required text editor** (ed/vi-lite/nano-lite). Bonus: awk subset, top.
Test programs: forktest pipetest sigtest mmaptest cowtest smptest stresstest, plus a usertests suite.

## Repository layout (adjust and document if needed)
```
Makefile README.md AGENTS.md CLAUDE.md knowledge.md
docs/    DECISIONS.md STATUS.md LOCKING.md SYSCALLS.md MEMORY.md ARCH.md SPEC.md
boot/ kernel/{arch/x86_64,mm,sched,fs,drivers,ipc,syscall,lib}/ libc/ userland/ initrd/ tools/ tests/
```

## Makefile
- Targets: `all run debug test iso clean distclean help`. Parallel-safe, incremental.
- `run`: `-smp $(SMP) -m 512M -serial stdio -display none -no-reboot -device isa-debug-exit`,
  `-accel kvm:tcg`; `GUI=1` opens a window.
- `debug`: QEMU `-s -S` plus `.gdbinit`.
- `test`: headless boot, runs the automated suite, parses PASS/FAIL markers, enforces timeouts,
  exits non-zero on failure.
- `-Wall -Wextra`; warning-free in the final state.

## Phases (verify each by running before moving on; commit + push after each)
1. Toolchain & boot: Makefile, bootloader, serial "hello", panic handler.
2. CPU core: GDT/TSS/IDT, exceptions, ACPI/MADT, LAPIC/IOAPIC, timer, keyboard IRQ.
3. Memory: PMM, VMM, heap, direct map, page-fault handler; in-kernel allocator stress test.
4. SMP: AP bring-up, per-CPU data, spinlocks, IPIs; every CPU prints "CPU n online"; contention test.
5. Scheduler & kernel threads: preemptive per-CPU scheduling, wait queues, sleep.
6. Userspace entry: syscalls, ELF loader, ring-3 `hello`.
7. Processes: fork (COW), exec, wait, exit, signals, pipes (forktest, cowtest, sigtest).
8. VFS + ramfs + initrd + devfs; `/init` runs from initrd.
9. TTY + libc + sh: interactive shell, pipelines, redirection, scripts.
10. Coreutils + procfs: each tool has at least one automated test.
11. Hardening: `stresstest` (forks, pipes, mmap/munmap, signals) on 1/2/4/8 CPUs, no panics,
    deadlocks, or leaks (PMM free-page accounting before/after).
12. Polish: docs, bonus features, warning-free build, README additions (first section only).

## Definition of Done
1. `make run` on a clean clone boots to a shell with N CPUs online (boot log + `/proc/cpuinfo`).
2. These work in the shell: `ls -l /`, `cat /proc/meminfo`, `echo hi | tr a-z A-Z`,
   `for i in 1 2 3; do echo $i; done`, `sleep 1 &`, `grep -r root /etc | sort | uniq -c | wc -l`,
   create/edit/delete files in tmpfs, run a shebang script.
3. fork+COW, exec, pipes, signals, job control (^C ^Z fg bg), and mmap demonstrably work.
4. `smptest` shows CPU-bound processes running in parallel on different CPUs.
5. `make test` passes; `stresstest` runs 10+ minutes at SMP=4 without panic or hang.
6. Warning-free build, documented, committed in logical commits and pushed to GitHub.
7. `docs/STATUS.md` honestly lists known gaps; README only extended under its first section.