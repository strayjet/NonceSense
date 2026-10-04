#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NonceSense Task 3 - constant-time verification + anti-spam
 * Target: STM32F103C8T6 / Blue Pill
 *
 * Clock: 8 MHz HSI
 * UART: USART1 PA9/PA10, 115200 8-N-1
 * LED:  PC13, active LOW
 * BTN:  PB12 -> EXTI12, falling edge, internal pull-up
 * Timer: TIM2, 50 ms interrupt
 *
 * FSM:
 *
 *   STANDBY --AUTH--> WAIT_BUTTON --button--> STANDBY
 *      ^                   |
 *      |                   +--10 s timeout--> STANDBY
 *      |
 *      +--violation 1..3--> DISCARD (1 s) --> STANDBY
 *      |
 *      +--violation 4+----> LOCKOUT (10 s) --> STANDBY
 *
 * UART RX is interrupt-driven. The USART1 ISR does only the minimum
 * work needed to move a received byte into the ring buffer.
 *
 * Security note:
 * The assignment's demo SECRET_KEY is intentionally fixed/public.
 * This is a protocol/firmware exercise, not production cryptography.
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

/* ARM Cortex-M3 DWT cycle counter. */
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004UL)
#define DEMCR       (*(volatile uint32_t *)0xE000EDFCUL)

#define LED_PIN             13U
#define BUTTON_PIN          12U
#define SECRET_KEY          0x12345678UL

#define RX_BUFFER_SIZE      128U
#define COMMAND_BUFFER_SIZE 64U

#define TIMER_TICK_MS       50U
#define AUTH_TIMEOUT_TICKS  200U  /* 10 s */
#define DISCARD_TICKS       20U   /* 1 s */
#define LOCKOUT_TICKS       200U  /* 10 s */

#define WAIT_BLINK_TICKS    2U    /* toggle every 100 ms = 5 Hz */
#define LOCKOUT_BLINK_TICKS 1U    /* toggle every 50 ms = 10 Hz */

typedef enum
{
    STATE_STANDBY = 0U,
    STATE_WAIT_BUTTON,
    STATE_DISCARD,
    STATE_LOCKOUT
} TokenState;

/* --------------------------- Ring buffer --------------------------- */

static volatile uint8_t rx_buffer[RX_BUFFER_SIZE];
static volatile uint16_t rx_head = 0U;
static volatile uint16_t rx_tail = 0U;
static volatile uint8_t rx_overflow = 0U;

static void uart_rx_flush(void)
{
    rx_tail = rx_head;
}

static bool uart_rx_pop(uint8_t *out)
{
    uint16_t tail = rx_tail;

    if (tail == rx_head)
    {
        return false;
    }

    *out = rx_buffer[tail];
    rx_tail = (uint16_t)((tail + 1U) % RX_BUFFER_SIZE);
    return true;
}

/* --------------------------- Global state -------------------------- */

static volatile TokenState state = STATE_STANDBY;
static volatile uint8_t button_pressed = 0U;
static volatile uint32_t auth_ticks = 0U;
static volatile uint32_t discard_ticks = 0U;
static volatile uint32_t lockout_ticks = 0U;
static volatile uint32_t blink_ticks = 0U;
static volatile uint32_t violation_count = 0U;

static uint32_t current_challenge = 0U;
static uint32_t expected_response = 0U;
static bool expected_valid = false;

/* ------------------------------ UART -------------------------------- */

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

static void uart_init(void)
{
    RCC_APB2ENR |= (1UL << 2) | (1UL << 4) | (1UL << 14);

    GPIOA_CRH &= ~(0xFUL << 4);
    GPIOA_CRH |=  (0xBUL << 4); /* PA9 TX */

    GPIOA_CRH &= ~(0xFUL << 8);
    GPIOA_CRH |=  (0x4UL << 8); /* PA10 RX */

    USART1_BRR = 0x45UL;

    /*
     * UE | TE | RE | RXNEIE.
     * RXNEIE = bit 5, so received bytes generate USART1 IRQs.
     */
    USART1_CR1 = (1UL << 13) | (1UL << 5) |
                 (1UL << 3) | (1UL << 2);

    /* USART1 IRQ = 37 -> NVIC ISER1 bit 5. */
    NVIC_ISER1 |= (1UL << 5);
}

void USART1_IRQHandler(void)
{
    uint32_t status = USART1_SR;

    if ((status & (1UL << 5)) != 0U)
    {
        uint8_t byte = (uint8_t)(USART1_DR & 0xFFUL);
        uint16_t head = rx_head;
        uint16_t next = (uint16_t)((head + 1U) % RX_BUFFER_SIZE);

        if (next == rx_tail)
        {
            rx_overflow = 1U;
        }
        else
        {
            rx_buffer[head] = byte;
            rx_head = next;
        }
    }
    else
    {
        /*
         * Reading SR above is intentional. No protocol work belongs
         * in this ISR; error recovery is handled by the next byte.
         */
    }
}

/* ------------------------------- LED -------------------------------- */

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

/* ------------------------------ Button ------------------------------- */

static void button_init(void)
{
    RCC_APB2ENR |= (1UL << 0) | (1UL << 3);

    /* PB12 input pull-up. */
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

    /* EXTI15_10 IRQ = 40 -> ISER1 bit 8. */
    NVIC_ISER1 |= (1UL << 8);
}

void EXTI15_10_IRQHandler(void)
{
    if ((EXTI_PR & (1UL << BUTTON_PIN)) != 0U)
    {
        EXTI_PR = (1UL << BUTTON_PIN);
        /* Requirement: button ISR only records the event. */
        button_pressed = 1U;
    }
}

/* ------------------------------- Timer -------------------------------- */

static void timer_init(void)
{
    RCC_APB1ENR |= (1UL << 0);

    /*
     * TIM2 clock = 8 MHz.
     * 8 MHz / (7999+1) = 1 kHz.
     * 1 kHz / (49+1) = 20 Hz => 50 ms.
     */
    TIM2_PSC = 7999U;
    TIM2_ARR = 49U;
    TIM2_EGR = 1UL;
    TIM2_SR &= ~1UL;
    TIM2_DIER |= 1UL;
    NVIC_ISER0 |= (1UL << 28);
    TIM2_CR1 |= 1UL;
}

void TIM2_IRQHandler(void)
{
    if ((TIM2_SR & 1UL) == 0U)
    {
        return;
    }

    TIM2_SR &= ~1UL;

    if (state == STATE_WAIT_BUTTON)
    {
        ++auth_ticks;

        ++blink_ticks;
        if (blink_ticks >= WAIT_BLINK_TICKS)
        {
            blink_ticks = 0U;
            led_toggle();
        }
    }
    else if (state == STATE_DISCARD)
    {
        if (discard_ticks > 0U)
        {
            --discard_ticks;
        }

        if (discard_ticks == 0U)
        {
            state = STATE_STANDBY;
            led_off();
        }
    }
    else if (state == STATE_LOCKOUT)
    {
        ++blink_ticks;
        if (blink_ticks >= LOCKOUT_BLINK_TICKS)
        {
            blink_ticks = 0U;
            led_toggle();
        }

        if (lockout_ticks > 0U)
        {
            --lockout_ticks;
        }

        if (lockout_ticks == 0U)
        {
            state = STATE_STANDBY;
            violation_count = 0U;
            led_off();
        }
    }
}

/* ------------------------------- CRC32 -------------------------------- */

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

/* ----------------------------- Hex parsing ---------------------------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex32(const char *s, uint32_t *out)
{
    uint32_t value = 0U;
    uint32_t i;

    for (i = 0U; i < 8U; ++i)
    {
        int digit = hex_value(s[i]);

        if (digit < 0)
        {
            return false;
        }

        value = (value << 4) | (uint32_t)digit;
    }

    *out = value;
    return true;
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

/* ----------------------- Constant-time verification ------------------- */

__attribute__((noinline))
bool constant_time_memcmp(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t difference = 0U;
    size_t i;

    for (i = 0U; i < len; ++i)
    {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }

    return difference == 0U;
}

/* -------------------------- Cycle counter ----------------------------- */

static void cycle_counter_init(void)
{
    DEMCR |= (1UL << 24); /* TRCENA */
    DWT_CYCCNT = 0U;
    DWT_CTRL |= 1UL;      /* CYCCNTENA */
}

static uint32_t cycle_counter_get(void)
{
    return DWT_CYCCNT;
}

static void verify_response(uint32_t supplied)
{
    uint8_t expected_bytes[4];
    uint8_t supplied_bytes[4];
    uint32_t start;
    uint32_t end;
    uint32_t cycles;
    bool result;
    char cycles_hex[9];

    if (!expected_valid)
    {
        uart_puts("FAIL\r\n");
        return;
    }

    expected_bytes[0] = (uint8_t)(expected_response >> 24);
    expected_bytes[1] = (uint8_t)(expected_response >> 16);
    expected_bytes[2] = (uint8_t)(expected_response >> 8);
    expected_bytes[3] = (uint8_t)expected_response;

    supplied_bytes[0] = (uint8_t)(supplied >> 24);
    supplied_bytes[1] = (uint8_t)(supplied >> 16);
    supplied_bytes[2] = (uint8_t)(supplied >> 8);
    supplied_bytes[3] = (uint8_t)supplied;

    start = cycle_counter_get();
    result = constant_time_memcmp(supplied_bytes, expected_bytes, 4U);
    end = cycle_counter_get();

    cycles = end - start;
    uint32_to_hex(cycles, cycles_hex);

    uart_puts("VERIFY_CYCLES:");
    uart_puts(cycles_hex);
    uart_puts("\r\n");
    uart_puts(result ? "OK\r\n" : "FAIL\r\n");
}

/* --------------------------- Anti-spam -------------------------------- */

static void register_violation(void)
{
    ++violation_count;
    uart_rx_flush();

    if (violation_count >= 4U)
    {
        state = STATE_LOCKOUT;
        expected_valid = false;
        lockout_ticks = LOCKOUT_TICKS;
        blink_ticks = 0U;
        led_off();

        uart_puts("[SECURITY ALERT] Malicious spam detected! "
                  "Hardware locked for 10 seconds.\r\n");
    }
    else
    {
        state = STATE_DISCARD;
        discard_ticks = DISCARD_TICKS;
        blink_ticks = 0U;
        led_off();
        uart_puts("ERR:INPUT_DISCARDED_1S\r\n");
    }
}

/* -------------------------- Command parser ---------------------------- */

static bool command_is_auth(const char *cmd, uint32_t length)
{
    return length == 13U &&
           cmd[0] == 'A' && cmd[1] == 'U' &&
           cmd[2] == 'T' && cmd[3] == 'H' && cmd[4] == ':';
}

static bool command_is_verify(const char *cmd, uint32_t length)
{
    return length == 15U &&
           cmd[0] == 'V' && cmd[1] == 'E' &&
           cmd[2] == 'R' && cmd[3] == 'I' &&
           cmd[4] == 'F' && cmd[5] == 'Y' && cmd[6] == ':';
}

static void process_command(const char *cmd, uint32_t length)
{
    uint32_t value;

    if (state == STATE_DISCARD || state == STATE_LOCKOUT)
    {
        return;
    }

    if (state == STATE_WAIT_BUTTON)
    {
        /* A complete command during the active operation is spam. */
        register_violation();
        return;
    }

    if (command_is_auth(cmd, length))
    {
        if (!parse_hex32(&cmd[5], &value))
        {
            register_violation();
            return;
        }

        current_challenge = value;
        expected_valid = false;
        auth_ticks = 0U;
        blink_ticks = 0U;
        state = STATE_WAIT_BUTTON;
        led_on();
        uart_puts("WAIT:PRESS_BUTTON\r\n");
        return;
    }

    if (command_is_verify(cmd, length))
    {
        if (!parse_hex32(&cmd[7], &value))
        {
            register_violation();
            return;
        }

        verify_response(value);
        return;
    }

    register_violation();
}

/* ------------------------------- Main --------------------------------- */

int main(void)
{
    char command[COMMAND_BUFFER_SIZE];
    uint32_t command_length = 0U;
    uint8_t c;

    uart_init();
    led_init();
    button_init();
    timer_init();
    cycle_counter_init();
    led_off();

    uart_puts("\r\nNonceSense Task 3 ready.\r\n");
    uart_puts("AUTH:<8-HEX-CHARS>\r\n");
    uart_puts("VERIFY:<8-HEX-CHARS>\r\n");

    while (1)
    {
        if (button_pressed != 0U)
        {
            button_pressed = 0U;

            if (state == STATE_WAIT_BUTTON && auth_ticks < AUTH_TIMEOUT_TICKS)
            {
                expected_response = compute_response(current_challenge);
                expected_valid = true;
                state = STATE_STANDBY;
                auth_ticks = 0U;
                blink_ticks = 0U;
                led_off();

                {
                    char response_hex[9];
                    uint32_to_hex(expected_response, response_hex);
                    uart_puts("RESP:");
                    uart_puts(response_hex);
                    uart_puts("\r\n");
                }
            }
            else if (state == STATE_STANDBY)
            {
                uart_puts("ERR:INVALID_STATE\r\n");
            }
        }

        if (state == STATE_WAIT_BUTTON &&
            auth_ticks >= AUTH_TIMEOUT_TICKS)
        {
            state = STATE_STANDBY;
            auth_ticks = 0U;
            blink_ticks = 0U;
            expected_valid = false;
            led_off();
            uart_puts("ERR:TIMEOUT\r\n");
        }

        if (rx_overflow != 0U)
        {
            rx_overflow = 0U;
            command_length = 0U;
            register_violation();
        }

        while (uart_rx_pop(&c))
        {
            if (state == STATE_DISCARD || state == STATE_LOCKOUT)
            {
                command_length = 0U;
                continue;
            }

            if (c == '\r')
            {
                continue;
            }

            if (c == '\n')
            {
                command[command_length] = '\0';
                process_command(command, command_length);
                command_length = 0U;
                continue;
            }

            if (command_length < (COMMAND_BUFFER_SIZE - 1U))
            {
                command[command_length++] = (char)c;
            }
            else
            {
                command_length = 0U;
                register_violation();
            }
        }
    }
}
