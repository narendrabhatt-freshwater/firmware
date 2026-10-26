ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    DTCM  (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
    AXI   (rw)  : ORIGIN = 0x24000000, LENGTH = 320K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);
_heap_limit = _estack - 16K;

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    dma PT_LOAD FLAGS(6);
}

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH :text
    .text :
    {
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH :text
    .ARM.extab : { *(.ARM.extab*) } > FLASH :text
    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH :text
    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH :text
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH :text
    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH :text
    .data :
    {
        . = ALIGN(8);
        _sdata = .;
        *(.data*)
        . = ALIGN(8);
        _edata = .;
    } > DTCM AT> FLASH :data
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        _sbss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        _ebss = .;
        _end = .;
        PROVIDE(end = .);
    } > DTCM :data
    .dma_buffer (NOLOAD) :
    {
        . = ALIGN(4);
        *(.dma_buffer*)
    } > AXI :dma
    .ARM.attributes 0 : { *(.ARM.attributes) }
    /DISCARD/ : { *(.note.GNU-stack) }
}

ASSERT(_end <= _heap_limit, "DTCM data overlaps the reserved stack")
