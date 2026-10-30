ENTRY(loader_entry)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x20004000, LENGTH = 0x00002000
}

SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.text.loader))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > SRAM

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
