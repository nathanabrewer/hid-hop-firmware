/*
 * Minimal boot test - tracks which init stage we reach
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>

/* nRF52840 GPIO P0 registers */
#define P0_OUTSET    (*(volatile uint32_t *)0x50000508)
#define P0_OUTCLR    (*(volatile uint32_t *)0x5000050C)
#define P0_DIRSET    (*(volatile uint32_t *)0x50000518)
#define P0_PIN_CNF(n) (*(volatile uint32_t *)(0x50000700 + (n)*4))

#define LED1 6
#define LED2 8

static void short_delay(void) {
    for (volatile int i = 0; i < 300000; i++) { __asm__("nop"); }
}

static void long_delay(void) {
    for (volatile int i = 0; i < 1500000; i++) { __asm__("nop"); }
}

/* Quick blinks followed by long pause - easier to count */
static void blink(int count) {
    for (int j = 0; j < count; j++) {
        P0_OUTCLR = (1 << LED1) | (1 << LED2);  /* ON */
        short_delay();
        P0_OUTSET = (1 << LED1) | (1 << LED2);  /* OFF */
        short_delay();
    }
    long_delay();  /* Long pause between stages */
}

/* Stage 1a: PRE_KERNEL_1 START (priority 0) */
static int stage1a(void) {
    P0_PIN_CNF(LED1) = 3;
    P0_PIN_CNF(LED2) = 3;
    P0_DIRSET = (1 << LED1) | (1 << LED2);
    P0_OUTSET = (1 << LED1) | (1 << LED2);  /* Off initially */
    blink(1);  /* 1 blink = priority 0 done */
    return 0;
}
SYS_INIT(stage1a, PRE_KERNEL_1, 0);

/* Priority 25 - before clock init (30) */
static int stage_p25(void) {
    blink(2);  /* 2 blinks = priority 25 done */
    return 0;
}
SYS_INIT(stage_p25, PRE_KERNEL_1, 25);

/* Priority 35 - after clock init (30) */
static int stage_p35(void) {
    blink(3);  /* 3 blinks = priority 35 done */
    return 0;
}
SYS_INIT(stage_p35, PRE_KERNEL_1, 35);

/* Stage 1b: PRE_KERNEL_1 END (priority 99) */
static int stage1b(void) {
    blink(4);  /* 4 blinks = PRE_KERNEL_1 finished */
    return 0;
}
SYS_INIT(stage1b, PRE_KERNEL_1, 99);

/* Stage 2: PRE_KERNEL_2 END (priority 99) */
static int stage2(void) {
    blink(5);  /* 5 blinks = PRE_KERNEL_2 finished */
    return 0;
}
SYS_INIT(stage2, PRE_KERNEL_2, 99);

/* Stage 3: POST_KERNEL */
static int stage3(void) {
    blink(6);  /* 6 blinks = POST_KERNEL */
    return 0;
}
SYS_INIT(stage3, POST_KERNEL, 99);

/* Stage 4: APPLICATION */
static int stage4(void) {
    blink(7);  /* 7 blinks = APPLICATION */
    return 0;
}
SYS_INIT(stage4, APPLICATION, 99);

int main(void) {
    blink(8);  /* 8 blinks = main reached! */

    /* SUCCESS - continuous slow blink */
    while (1) {
        P0_OUTCLR = (1 << LED1) | (1 << LED2);
        k_sleep(K_MSEC(500));
        P0_OUTSET = (1 << LED1) | (1 << LED2);
        k_sleep(K_MSEC(500));
    }
    return 0;
}
