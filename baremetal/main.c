/* Bare metal LED blink - no OS, no drivers */

#define GPIO_P0_BASE    0x50000000UL
#define GPIO_P0_DIRSET  (*(volatile unsigned int *)(GPIO_P0_BASE + 0x518))
#define GPIO_P0_OUTSET  (*(volatile unsigned int *)(GPIO_P0_BASE + 0x508))
#define GPIO_P0_OUTCLR  (*(volatile unsigned int *)(GPIO_P0_BASE + 0x50C))

/* LED on P0.6 and P0.8 - try both */
#define LED_MASK ((1 << 6) | (1 << 8))

void delay(void) {
    for (volatile int i = 0; i < 200000; i++);
}

void main(void) {
    GPIO_P0_DIRSET = LED_MASK;

    while (1) {
        GPIO_P0_OUTCLR = LED_MASK;  /* ON (active low) */
        delay();
        GPIO_P0_OUTSET = LED_MASK;  /* OFF */
        delay();
    }
}

/* Minimal vector table */
extern unsigned int _estack;

void Default_Handler(void) {
    while (1);
}

__attribute__((section(".isr_vector")))
void (* const vectors[])(void) = {
    (void (*)(void))&_estack,  /* Initial stack pointer */
    (void (*)(void))main,      /* Reset handler */
    Default_Handler,           /* NMI Handler */
    Default_Handler,           /* HardFault Handler */
};
