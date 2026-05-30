/*
 * Minimal test - just blink LEDs to verify boot stages
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

/* nRF52840 GPIO P0 registers - direct access */
#define P0_OUTSET    (*(volatile uint32_t *)0x50000508)
#define P0_OUTCLR    (*(volatile uint32_t *)0x5000050C)
#define P0_DIRSET    (*(volatile uint32_t *)0x50000518)
#define P0_PIN_CNF(n) (*(volatile uint32_t *)(0x50000700 + (n)*4))

/* Raytac MDBT50Q-CX LEDs on P0.6 and P0.8 - active low */
#define LED1 6
#define LED2 8

static void delay(void) {
    for (volatile int i = 0; i < 800000; i++) { __asm__("nop"); }
}

static void blink(int count) {
    for (int j = 0; j < count; j++) {
        P0_OUTCLR = (1 << LED1) | (1 << LED2);  /* ON */
        delay();
        P0_OUTSET = (1 << LED1) | (1 << LED2);  /* OFF */
        delay();
    }
    delay(); delay();  /* Pause between sets */
}

/* PRE_KERNEL_1 - earliest init stage */
static int stage1_pre_kernel_1(void) {
    P0_PIN_CNF(LED1) = 3;  /* Output, standard drive */
    P0_PIN_CNF(LED2) = 3;
    P0_DIRSET = (1 << LED1) | (1 << LED2);
    P0_OUTSET = (1 << LED1) | (1 << LED2);  /* LEDs off initially */
    blink(1);
    return 0;
}
SYS_INIT(stage1_pre_kernel_1, PRE_KERNEL_1, 0);

/* PRE_KERNEL_2 - drivers init */
static int stage2_pre_kernel_2(void) {
    blink(2);
    return 0;
}
SYS_INIT(stage2_pre_kernel_2, PRE_KERNEL_2, 99);

/* POST_KERNEL - kernel services ready */
static int stage3_post_kernel(void) {
    blink(3);
    return 0;
}
SYS_INIT(stage3_post_kernel, POST_KERNEL, 99);

/* APPLICATION - apps can init */
static int stage4_application(void) {
    blink(4);
    return 0;
}
SYS_INIT(stage4_application, APPLICATION, 99);

int main(void)
{
    /* 5 blinks = main() reached */
    blink(5);

    /* Continuous blink to show we're running */
    while (1) {
        P0_OUTCLR = (1 << LED1) | (1 << LED2);  /* ON */
        k_sleep(K_MSEC(500));
        P0_OUTSET = (1 << LED1) | (1 << LED2);  /* OFF */
        k_sleep(K_MSEC(500));
    }
    return 0;
}
