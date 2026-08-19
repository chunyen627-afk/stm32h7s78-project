/* Minimal Cortex-M startup for running the core tests under QEMU. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void)
{
    /* Copy initialised data from flash to RAM. */
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    /* Zero the bss. */
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }

    /* rdimon needs this to wire up semihosting stdio; normally the library's
     * own crt0 calls it, which -nostartfiles skips. */
    extern void initialise_monitor_handles(void);
    initialise_monitor_handles();

    /* No C++ constructors here, so __libc_init_array is not needed. */
    main();

    /* Semihosting exit so QEMU terminates cleanly. */
    register int r0 __asm__("r0") = 0x18;      /* SYS_EXIT */
    register int r1 __asm__("r1") = 0x20026;   /* ADP_Stopped_ApplicationExit */
    __asm__ volatile("bkpt #0xAB" :: "r"(r0), "r"(r1));
    for (;;) { }
}

static void Default_Handler(void)
{
    for (;;) { }
}

/* Vector table: initial SP, reset vector, then faults. */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Default_Handler,   /* NMI */
    Default_Handler,   /* HardFault */
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
    0, 0, 0, 0,
    Default_Handler,   /* SVC */
    Default_Handler,   /* DebugMon */
    0,
    Default_Handler,   /* PendSV */
    Default_Handler,   /* SysTick */
};
