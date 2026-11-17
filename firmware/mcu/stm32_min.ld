/*
 * Minimal linker script for STM32 parts: flash at 0x08000000, SRAM at
 * 0x20000000. The sizes are the smallest of the common Nucleo-64 boards
 * (F401RE: 512K/96K, F411RE: 512K/128K, F446RE: 512K/128K, L476RG: 1M/96K)
 * so one image runs on all of them.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ : { *(.ARM.attributes) }
}
