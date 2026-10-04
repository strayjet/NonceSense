#include <stdint.h>

/*
 * NonceSense Task 0 - bare-metal blinky
 * Target: STM32F103C8T6 / Blue Pill
 *
 * Requirement: no headers except <stdint.h>, no CMSIS, no HAL.
 * All peripheral addresses below come directly from the STM32F103
 * reference manual.
 *
 * LED: PC13, active LOW.
 * Clock: internal HSI, 8 MHz after reset.
 */

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define GPIOC_CRH   (*(volatile uint32_t *)0x40011004UL)
#define GPIOC_ODR   (*(volatile uint32_t *)0x4001100CUL)

#define GPIOCEN     (1UL << 4)
#define LED_PIN     13U

static void delay_half_second(void)
{
    /*
     * Deliberately approximate. The STM32F103 is left at its 8 MHz
     * HSI reset clock, so this gives a visible ~1 Hz blink.
     */
    volatile uint32_t i;
    for (i = 0U; i < 500000U; ++i)
    {
        __asm volatile ("nop");
    }
}

int main(void)
{
    RCC_APB2ENR |= GPIOCEN;

    /* PC13: MODE=10 (2 MHz), CNF=00 (general-purpose push-pull). */
    GPIOC_CRH &= ~(0xFUL << 20);
    GPIOC_CRH |=  (0x2UL << 20);

    /* Blue Pill LED is active-low: HIGH = off. */
    GPIOC_ODR |= (1UL << LED_PIN);

    while (1)
    {
        GPIOC_ODR ^= (1UL << LED_PIN);
        delay_half_second();
    }
}
