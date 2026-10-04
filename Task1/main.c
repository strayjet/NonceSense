#include <stdint.h>

/*
 * NonceSense Task 1 - UART challenge/response
 * Target: STM32F103C8T6, 8 MHz HSI
 *
 * Pin map:
 *   PA9  USART1_TX
 *   PA10 USART1_RX
 *   PC13 status LED (active LOW)
 *
 * Protocol:
 *   Host -> AUTH:<8-HEX-CHARS>\n
 *   Token -> RESP:<8-HEX-CHARS>\n
 *
 * Response = CRC32(challenge_bytes || SECRET_KEY_bytes), with both
 * 32-bit values encoded most-significant byte first.
 *
 * The demo key is intentionally public because this is an assignment
 * demonstrating the protocol, not production cryptographic storage.
 */

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOA_CRH   (*(volatile uint32_t *)0x40010804UL)
#define GPIOC_CRH   (*(volatile uint32_t *)0x40011004UL)
#define GPIOC_ODR   (*(volatile uint32_t *)0x4001100CUL)

#define USART1_SR   (*(volatile uint32_t *)0x40013800UL)
#define USART1_DR   (*(volatile uint32_t *)0x40013804UL)
#define USART1_BRR  (*(volatile uint32_t *)0x40013808UL)
#define USART1_CR1  (*(volatile uint32_t *)0x4001380CUL)

#define LED_PIN     13U
#define SECRET_KEY  0x12345678UL

static void uart_init(void)
{
    /* GPIOA, GPIOC and USART1 clocks. */
    RCC_APB2ENR |= (1UL << 2) | (1UL << 4) | (1UL << 14);

    /* PA9: alternate-function push-pull, 50 MHz. */
    GPIOA_CRH &= ~(0xFUL << 4);
    GPIOA_CRH |=  (0xBUL << 4);

    /* PA10: floating input. */
    GPIOA_CRH &= ~(0xFUL << 8);
    GPIOA_CRH |=  (0x4UL << 8);

    /* PCLK2 = 8 MHz, 115200 8-N-1 => BRR = 0x45. */
    USART1_BRR = 0x45UL;
    USART1_CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2);
}

static void uart_putc(char c)
{
    while ((USART1_SR & (1UL << 7)) == 0U)
    {
    }
    USART1_DR = (uint32_t)(uint8_t)c;
}

static char uart_getc(void)
{
    while ((USART1_SR & (1UL << 5)) == 0U)
    {
    }
    return (char)(USART1_DR & 0xFFUL);
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
    GPIOC_CRH &= ~(0xFUL << 20);
    GPIOC_CRH |=  (0x2UL << 20);
    GPIOC_ODR |= (1UL << LED_PIN);
}

static void led_toggle(void)
{
    GPIOC_ODR ^= (1UL << LED_PIN);
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
        if (digit < 0)
        {
            return 0U;
        }
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
    uint8_t i;
    uint32_t values[2] = { challenge, SECRET_KEY };

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

static uint8_t valid_auth(const char *cmd, uint32_t length)
{
    uint32_t unused;

    return length == 13U &&
           cmd[0] == 'A' && cmd[1] == 'U' &&
           cmd[2] == 'T' && cmd[3] == 'H' &&
           cmd[4] == ':' &&
           parse_hex32(&cmd[5], &unused);
}

int main(void)
{
    char command[64];
    uint32_t length;
    uint32_t challenge;
    uint32_t response;
    char response_hex[9];

    uart_init();
    led_init();

    uart_puts("\r\nNonceSense Task 1 ready.\r\n");
    uart_puts("AUTH:<8-HEX-CHARS>\r\n");

    while (1)
    {
        length = 0U;

        while (1)
        {
            char c = uart_getc();

            if (c == '\n')
            {
                break;
            }

            if (c == '\r')
            {
                continue;
            }

            if (length < (sizeof(command) - 1U))
            {
                command[length++] = c;
            }
            else
            {
                length = 0U;
                uart_puts("ERR:LINE_TOO_LONG\r\n");
                break;
            }
        }

        if (length == 0U)
        {
            continue;
        }

        if (!valid_auth(command, length))
        {
            uart_puts("ERR:INVALID_COMMAND\r\n");
            continue;
        }

        (void)parse_hex32(&command[5], &challenge);
        response = compute_response(challenge);
        uint32_to_hex(response, response_hex);

        uart_puts("RESP:");
        uart_puts(response_hex);
        uart_puts("\r\n");

        led_toggle();
    }
}
