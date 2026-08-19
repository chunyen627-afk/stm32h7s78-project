/* Linker script for QEMU mps2-an500 (Cortex-M7, matches the H7S7 core).
 * Code in ZBT SRAM1 at 0x00000000, data in SRAM at 0x20000000 (16MB). */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
ENTRY(Reset_Handler)
SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : {
    *(.text*) *(.rodata*) *(.glue_7) *(.glue_7t)
    . = ALIGN(4); _etext = .;
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
  .bss : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  .heap (NOLOAD) : { . = ALIGN(8); end = .; PROVIDE(_end = .); . = . + 64K; . = ALIGN(8); } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
