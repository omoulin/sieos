ENTRY(_start)

KERNEL_VBASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;
    _kernel_phys_start = .;

    .boot : {
        KEEP(*(.multiboot2))
        *(.boot.text)
        *(.boot.data)
    }
    .boot.bss (NOLOAD) : ALIGN(4096) {
        *(.boot.bss)
    }

    . += KERNEL_VBASE;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VBASE) {
        *(.text .text.*)
    }
    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VBASE) {
        *(.rodata .rodata.*)
    }
    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VBASE) {
        *(.data .data.*)
    }
    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VBASE) {
        *(COMMON)
        *(.bss .bss.*)
    }
    . = ALIGN(4096);
    _kernel_end = .;
    _kernel_phys_end = . - KERNEL_VBASE;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
