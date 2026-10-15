/* Overlay (make_overlay.py): one block at OVL_BASE (a free gap of the base image, given with --defsym), zeroed data
 * included so a single plain LDR block loads everything. The next_* symbols (the base image's hooks) come from
 * --defsym too. */
SECTIONS
{
    . = OVL_BASE;
    .blob : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
        __ovl_bss = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        LONG(0x6C766F21)                /* "!ovl": a trailing word so objcopy -O binary also emits the zeroed .bss */
    }
    __ovl_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.BFIN.*) }
}
