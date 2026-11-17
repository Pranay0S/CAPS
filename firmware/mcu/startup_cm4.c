/* Minimal Cortex-M4F startup: vector table, .data/.bss init, FPU enable. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void);
static void Default_Handler(void)
{
    for (;;) {
    }
}

#define SCB_CPACR (*(volatile uint32_t *)0xE000ED88u)

void Reset_Handler(void)
{
    /* Enable CP10/CP11 (FPU) before any float instruction runs. */
    SCB_CPACR |= 0xFu << 20;
    __asm volatile("dsb\n isb");

    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;)
        *d++ = *s++;
    for (uint32_t *d = &_sbss; d < &_ebss;)
        *d++ = 0;

    main();
    Default_Handler();
}

/* Core exceptions only: no peripheral interrupts are used. */
__attribute__((section(".isr_vector"), used))
static void (*const vectors[16])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler, /* NMI */
    Default_Handler, /* HardFault */
    Default_Handler, /* MemManage */
    Default_Handler, /* BusFault */
    Default_Handler, /* UsageFault */
    0, 0, 0, 0,
    Default_Handler, /* SVC */
    Default_Handler, /* DebugMon */
    0,
    Default_Handler, /* PendSV */
    Default_Handler, /* SysTick */
};

/* newlib-nano's libm may reference these; nothing here allocates or does I/O. */
int *__errno(void)
{
    static int e;
    return &e;
}
