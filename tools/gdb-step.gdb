# Batch GDB script: dump the raw GDT around the TSS right before `ltr`, using
# absolute addresses (GDB's arithmetic on the packed array is unreliable).
set pagination off
set confirm off

target remote :1234

hbreak gdt_init
continue
hbreak *0xffffffff8000025e
continue

printf "--- at ltr ---\n"
printf "gdt[0] null       = %016lx\n", *(unsigned long long *)0xffffffff8000e078
printf "gdt[1] kernel code= %016lx\n", *(unsigned long long *)0xffffffff8000e078
printf "gdt[2] kernel data= %016lx\n", *(unsigned long long *)0xffffffff8000e080
printf "gdt[5] TSS low    = %016lx\n", *(unsigned long long *)0xffffffff8000e098
printf "gdt[6] TSS high   = %016lx\n", *(unsigned long long *)0xffffffff8000e0a0
printf "gdt[7] unused     = %016lx\n", *(unsigned long long *)0xffffffff8000e0a8
printf "gdt_ptr.limit     = %u\n", *(unsigned short *)0xffffffff8000e0b0
printf "gdt_ptr.base      = %lx\n", *(unsigned long long *)0xffffffff8000e0b2
quit