--entry_point=loader_entry
--stack_size=0

MEMORY
{
    LOADER (RWX) : origin = 0x20200000, length = 0x00002000
}

SECTIONS
{
    .text       : palign(8) {} > LOADER
    .TI.ramfunc : palign(8) {} > LOADER
    .const      : palign(8) {} > LOADER
    .rodata     : palign(8) {} > LOADER
    .data       : palign(8) {} > LOADER
    .bss        : palign(8) {} > LOADER
}
