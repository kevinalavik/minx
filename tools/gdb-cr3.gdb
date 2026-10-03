# Batch GDB script: is the kernel image really at physical 0?
set pagination off
set confirm off

target remote :1234

hbreak vmm_init
continue

set $hhdm = 0xffff800000000000
printf "--- in vmm_init, reading physical memory through the direct map ---\n"
printf "image vaddr %016lx\n", $pc

printf "phys 0x000000 (image+0x0000): %02x %02x %02x %02x\n", \
  *(unsigned char *)($hhdm + 0x000000), *(unsigned char *)($hhdm + 0x000001), \
  *(unsigned char *)($hhdm + 0x000002), *(unsigned char *)($hhdm + 0x000003)
printf "phys 0x000c00 (image+0xc000): %02x %02x %02x %02x\n", \
  *(unsigned char *)($hhdm + 0x00c000), *(unsigned char *)($hhdm + 0x00c001), \
  *(unsigned char *)($hhdm + 0x00c002), *(unsigned char *)($hhdm + 0x00c003)
printf "phys 0x001000 (image+0x1000): %02x %02x %02x %02x\n", \
  *(unsigned char *)($hhdm + 0x001000), *(unsigned char *)($hhdm + 0x001001), \
  *(unsigned char *)($hhdm + 0x001002), *(unsigned char *)($hhdm + 0x001003)
printf "phys 0x0062140 (near vmm_init+0x610): %02x %02x %02x %02x\n", \
  *(unsigned char *)($hhdm + 0x0062140), *(unsigned char *)($hhdm + 0x0062141), \
  *(unsigned char *)($hhdm + 0x0062142), *(unsigned char *)($hhdm + 0x0062143)

# Search the first 4 MiB for the kernel's entry-point bytes, to find where the
# image really is in physical memory.
set $i = 0
set $found = 0
while $i < 0x400000 && $found == 0
  # the first bytes of .text: the first instruction of _start
  if (*(unsigned char *)($hhdm + $i) == 0xfa && *(unsigned char *)($hhdm + $i + 1) == 0x0f)
    printf "candidate at physical %#lx: %02x %02x %02x %02x\n", $i, \
      *(unsigned char *)($hhdm + $i), *(unsigned char *)($hhdm + $i + 1), \
      *(unsigned char *)($hhdm + $i + 2), *(unsigned char *)($hhdm + $i + 3)
    set $found = 1
  end
  set $i = $i + 0x1000
end
if $found == 0
  printf "no candidate in the first 4 MiB\n"
end

quit