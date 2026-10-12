# SYSCALLS

The user/kernel boundary. Everything here is one enum in `kernel/include/syscall.h` and
nothing else -- there is no second list, and there must not become one.

## The door

Vector 128, reached from ring 3 with `int $0x80`. `syscall`/`sysretq` are not used: this
kernel already has an interrupt path with a TSS stack switch on it, and reusing it costs
nothing in new machinery and cannot get the privilege transition subtly wrong.

Arguments follow the SysV AMD64 convention, because the compiler generates it:

| position | register |
|----------|----------|
| call number | `rax` |
| 1 | `rdi` |
| 2 | `rsi` |
| 3 | `rdx` |
| 4 | `rcx` |
| 5 | `r8` |
| 6 | `r9` |
| 7+ | stack |
| result | `rax` |

`rax` is the `sysretq` return register, so it cannot also be an argument: the seventh
argument and onwards go on the stack.

## Two rules

**A failed call returns `-errno` in `rax`; it does not set `errno` and return -1.** The libc's
`__syscall_ret()` in `<sys/sysdep.h>` converts between the two conventions, in exactly one
place, so the conversion cannot be done twice with two different ideas about it. The
conversion is `r < 0 && r > -4095` -- a call that succeeds and returns -1 must stay -1, so
the boundary has to be somewhere above every plausible result and below every errno.

**Pass every argument the kernel reads.** The registers a call does not name are *scratch*
registers: the ABI promises nothing about them, and the compiler will put anything in them.
So a three-argument call given two arguments supplies the third out of whatever the caller
was using.

This is not hypothetical. `wait()` was extended from one argument to three (status pointer,
pid selector, options word) to give a shell job control. The old two-argument caller left
`rdx` undefined, bit 0 of it came back set, `WNOHANG` came back on, and `wait()` returned
"nothing yet" for a child that had already exited -- in `phase7-cow`, which was passing
before the change and failing after it. `user/common/minx_user.h`'s `minx_wait()` now passes
all three, with the reasoning at the call.

## The numbers

Generated, not written out twice. `tools/gen-syscall-table.sh` parses the enum and emits
`build/include/minx_syscall_table.h` (for C) and `build/include/minx_syscall.inc` (for
assembly). `user/common/start.S` is assembly and cannot see a C header's macros, which is
why there are two outputs from one pass.

They are generated because the copy is what breaks. Renumbering the enum once updated
`minx_user.h` and left `start.S`'s literal `SYS_EXIT` alone; `movl $4, %eax` became
`lseek`, so every program printed its last line and then spun in the entry stub's "exit does
not return either" loop for ever. Three tests failed with three unrelated-looking messages
and the common cause was one number in an assembly file.

## The groups

| range | what |
|-------|------|
| 0–12   | descriptors: read, write, open, close, lseek, fstat, stat, lstat, fsync, readlink, getdents, dup, dup2 |
| 16–32  | the filesystem: mkdir, rmdir, unlink, rename, link, symlink, chmod, chown, truncate, ftruncate, utimes, chdir, getcwd, umask, mount, umount, access |
| 33–34  | fcntl, ioctl |
| 40–62  | processes: exit, brk, writev, getpid, getppid, fork, wait, pipe, exec, setsid, setpgid, getpgid, uname, getuid, geteuid, setuid, getgid, nanosleep, sched_yield, times, getrusage, reboot, pause |
| 64–68  | signals: kill, sigaction, sigreturn, sigprocmask, sigpending |
| 72–74  | memory: mmap, munmap, mprotect |
| 96     | `SYS_MAX`: the bound the dispatcher checks, not a call |

Gaps are deliberate. The groups are contiguous, so a new call in a group is the next number
in it, and a number that is *not* in a group is a number nothing implements -- which is what
`ENOSYS` is for, and what `hello` checks by calling one.

## Pointer arguments

Every user pointer goes through `copy_from_user`/`copy_to_user` with fault handling, or
through `syscall_user_range_ok()` first. A kernel address passed as a path comes back
`EFAULT` rather than being read, and `phase6-pointers` checks the boundary: the kernel image,
the direct map, the top of user space, `~0`, and a wrapping range are all refused, and an
empty range at a legal address is not.

## Signals

`SYS_SIGRETURN` is not called by any program. A handler returns into a three-instruction
trampoline the kernel writes on the signal frame (`mov eax, SYS_SIGRETURN; int $0x80`), so a
handler is an ordinary C function that `return`s from. The trampoline is mapped read-only at
`SIGNAL_TRAMPOLINE_ADDR` in every user address space, so it is one copy in the machine.