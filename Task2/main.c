#include <stdint.h>

/*
 * NonceSense Task 2 - physical user presence
 * Target: STM32F103C8T6 / Blue Pill, 8 MHz HSI
 *
 * PA9/PA10  USART1 TX/RX, 115200 8-N-1
 * PC13      LED, active LOW
 * PB12      push button, internal pull-up, active LOW
 * TIM2      100 ms update interrupt
 *
 * FSM:
 *
 *                 valid AUTH
 *       +----------------------------+
 *       |                            v
 *   [STANDBY] ----------------> [WAIT_BUTTON]
 *       ^                            |
 *       |              +-------------+-------------+
 *       |              |                           |
 *       |           button                       10 s
 *       |              |                           |
 *       +--------------+---------------------------+
 *
 * The button ISR only clears the EXTI pending bit and sets
 * button_pressed. All protocol work happens in main().
 */

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB1ENR (*(volatile uint32_t *)0x4002101CUL)

#define GPIOA_CRH   (*(volatile uint32_t *)0x40010804UL)
#define GPIOB_CRH   (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_ODR   (*(volatile uint32_t *)0x40010C0CUL)
#define GPIOC_CRH   (*(volatile uint32_t *)0x40011004UL)
#define GPIOC_ODR   (*(volatile uint32_t *)0x4001100CUL)

#define AFIO_EXTICR4 (*(volatile uint32_t *)0x40010014UL)

#define EXTI_IMR    (*(volatile uint32_t *)0x40010400UL)
#define EXTI_RTSR   (*(volatile uint32_t *)0x40010408UL)
#define EXTI_FTSR   (*(volatile uint32_t *)0x4001040CUL)
#define EXTI_PR     (*(volatile uint32_t *)0x40010414UL)

#define USART1_SR   (*(volatile uint32_t *)0x40013800UL)
#define USART1_DR   (*(volatile uint32_t *)0x40013804UL)
#define USART1_BRR  (*(volatile uint32_t *)0x40013808UL)
#define USART1_CR1  (*(volatile uint32_t *)0x4001380CUL)

#define TIM2_CR1    (*(volatile uint32_t *)0x40000000UL)
#define TIM2_DIER   (*(volatile uint32_t *)0x4000000CUL)
#define TIM2_SR     (*(volatile uint32_t *)0x40000010UL)
#define TIM2_EGR    (*(volatile uint32_t *)0x40000014UL)
#define TIM2_PSC    (*(volatile uint32_t *)0x40000028UL)
#define TIM2_ARR    (*(volatile uint32_t *)0x4000002CUL)

#define NVIC_ISER0  (*(volatile uint32_t *)0xE000E100UL)
#define NVIC_ISER1  (*(volatile uint32_t *)0xE000E104UL)

#define LED_PIN             13U
#define BUTTON_PIN          12U
#define SECRET_KEY          0x12345678UL
#define AUTH_TIMEOUT_TICKS  100U       /* 100 x 100 ms = 10 s */

typedef enum
{
    STATE_STANDBY = 0U,
    STATE_WAIT_BUTTON = 1U
} TokenState;

static volatile TokenState state = STATE_STANDBY;
static volatile uint8_t button_pressed = 0U;
static volatile uint8_t timeout_pending = 0U;
static volatile uint32_t elapsed_ticks = 0U;

static uint32_t current_challenge = 0U;

static void uart_init(void)
{
    RCC_APB2ENR |= (1UL << 2) | (1UL << 4) | (1UL << 14);

    GPIOA_CRH &= ~(0xFUL << 4);
    GPIOA_CRH |=  (0xBUL << 4);
    GPIOA_CRH &= ~(0xFUL << 8);
    GPIOA_CRH |=  (0x4UL << 8);

    USART1_BRR = 0x45UL;
    USART1_CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2);
}

static uint8_t uart_available(void)
{
    return (USART1_SR & (1UL << 5)) != 0U;
}

static char uart_getc(void)
{
    return (char)(USART1_DR & 0xFFUL);
}

static void uart_putc(char c)
{
    while ((USART1_SR & (1UL << 7)) == 0U)
    {
    }
    USART1_DR = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        uart_putc(*s++);
    }
}

static void led_init(void)
{
    RCC_APB2ENR |= (1UL << 4);
    GPIOC_CRH &= ~(0xFUL << 20);
    GPIOC_CRH |=  (0x2UL << 20);
    GPIOC_ODR |= (1UL << LED_PIN);
}

static void led_on(void)  { GPIOC_ODR &= ~(1UL << LED_PIN); }
static void led_off(void) { GPIOC_ODR |=  (1UL << LED_PIN); }
static void led_toggle(void) { GPIOC_ODR ^= (1UL << LED_PIN); }

static void button_init(void)
{
    RCC_APB2ENR |= (1UL << 0) | (1UL << 3);

    /* PB12 input with pull-up/pull-down; ODR=1 selects pull-up. */
    GPIOB_CRH &= ~(0xFUL << 16);
    GPIOB_CRH |=  (0x8UL << 16);
    GPIOB_ODR |= (1UL << BUTTON_PIN);

    /* EXTI12 source = GPIOB. */
    AFIO_EXTICR4 &= ~0xFUL;
    AFIO_EXTICR4 |=  0x1UL;

    EXTI_IMR  |=  (1UL << BUTTON_PIN);
    EXTI_RTSR &= ~(1UL << BUTTON_PIN);
    EXTI_FTSR |=  (1UL << BUTTON_PIN);
    EXTI_PR = (1UL << BUTTON_PIN);

    /* EXTI15_10 IRQ = 40 -> NVIC ISER1 bit 8. */
    NVIC_ISER1 |= (1UL << 8);
}

void EXTI15_10_IRQHandler(void)
{
    if ((EXTI_PR & (1UL << BUTTON_PIN)) != 0U)
    {
        EXTI_PR = (1UL << BUTTON_PIN);
        button_pressed = 1U;
    }
}

static void timer_init(void)
{
    RCC_APB1ENR |= (1UL << 0);

    /* 8 MHz / 8000 = 1 kHz; /100 = 10 Hz => 100 ms tick. */
    TIM2_PSC = 7999U;
    TIM2_ARR = 99U;
    TIM2_EGR = 1UL;
    TIM2_SR &= ~(1UL << 0);
    TIM2_DIER |= 1UL;
    NVIC_ISER0 |= (1UL << 28);
    TIM2_CR1 |= 1UL;
}

void TIM2_IRQHandler(void)
{
    if ((TIM2_SR & 1UL) != 0U)
    {
        TIM2_SR &= ~1UL;

        if (state == STATE_WAIT_BUTTON)
        {
            led_toggle();
            ++elapsed_ticks;

            if (elapsed_ticks >= AUTH_TIMEOUT_TICKS)
            {
                timeout_pending = 1U;
            }
        }
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static uint8_t parse_hex32(const char *s, uint32_t *out)
{
    uint32_t value = 0U;
    uint32_t i;

    for (i = 0U; i < 8U; ++i)
    {
        int digit = hex_value(s[i]);
        if (digit < 0) return 0U;
        value = (value << 4) | (uint32_t)digit;
    }

    *out = value;
    return 1U;
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    uint32_t bit;

    crc ^= (uint32_t)byte;
    for (bit = 0U; bit < 8U; ++bit)
    {
        crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL)
                         : (crc >> 1);
    }
    return crc;
}

static uint32_t compute_response(uint32_t challenge)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t values[2] = { challenge, SECRET_KEY };
    uint32_t i;

    for (i = 0U; i < 2U; ++i)
    {
        crc = crc32_update(crc, (uint8_t)(values[i] >> 24));
        crc = crc32_update(crc, (uint8_t)(values[i] >> 16));
        crc = crc32_update(crc, (uint8_t)(values[i] >> 8));
        crc = crc32_update(crc, (uint8_t)values[i]);
    }

    return crc ^ 0xFFFFFFFFUL;
}

static char hex_digit(uint8_t value)
{
    return (value < 10U) ? (char)('0' + value)
                         : (char)('A' + value - 10U);
}

static void uint32_to_hex(uint32_t value, char *out)
{
    int i;

    for (i = 7; i >= 0; --i)
    {
        out[i] = hex_digit((uint8_t)(value & 0xFUL));
        value >>= 4;
    }
    out[8] = '\0';
}

static void start_auth(uint32_t challenge)
{
    current_challenge = challenge;
    elapsed_ticks = 0U;
    timeout_pending = 0U;
    state = STATE_WAIT_BUTTON;
    led_on();
    uart_puts("WAIT:PRESS_BUTTON\r\n");
}

static void finish_auth(void)
{
    uint32_t response = compute_response(current_challenge);
    char response_hex[9];

    uint32_to_hex(response, response_hex);
    state = STATE_STANDBY;
    elapsed_ticks = 0U;
    timeout_pending = 0U;
    led_off();

    uart_puts("RESP:");
    uart_puts(response_hex);
    uart_puts("\r\n");
}

static void process_command(const char *cmd, uint32_t length)
{
    uint32_t challenge;

    if (state != STATE_STANDBY)
    {
        uart_puts("ERR:INVALID_STATE\r\n");
        return;
    }

    if (length != 13U ||
        cmd[0] != 'A' || cmd[1] != 'U' ||
        cmd[2] != 'T' || cmd[3] != 'H' || cmd[4] != ':' ||
        !parse_hex32(&cmd[5], &challenge))
    {
        uart_puts("ERR:INVALID_COMMAND\r\n");
        return;
    }

    start_auth(challenge);
}

int main(void)
{
    char command[64];
    uint32_t length = 0U;

    uart_init();
    led_init();
    button_init();
    timer_init();
    led_off();

    uart_puts("\r\nNonceSense Task 2 ready.\r\n");
    uart_puts("AUTH:<8-HEX-CHARS>\r\n");

    while (1)
    {
        if (button_pressed != 0U)
        {
            button_pressed = 0U;

            if (state == STATE_WAIT_BUTTON &&
                timeout_pending == 0U &&
                elapsed_ticks < AUTH_TIMEOUT_TICKS)
            {
                finish_auth();
            }
            else
            {
                uart_puts("ERR:INVALID_STATE\r\n");
            }
        }

        if (timeout_pending != 0U)
        {
            timeout_pending = 0U;

            if (state == STATE_WAIT_BUTTON)
            {
                state = STATE_STANDBY;
                elapsed_ticks = 0U;
                led_off();
                uart_puts("ERR:TIMEOUT\r\n");
            }
        }

        if (uart_available())
        {
            char c = uart_getc();

            if (c == '\r')
            {
                continue;
            }

            if (c == '\n')
            {
                command[length] = '\0';
                process_command(command, length);
                length = 0U;
            }
            else if (length < (sizeof(command) - 1U))
            {
                command[length++] = c;
            }
            else
            {
                length = 0U;
                uart_puts("ERR:LINE_TOO_LONG\r\n");
            }
        }
    }
}
