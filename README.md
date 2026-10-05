# NonceSense — Bare-Metal Hardware Authentication Token

A terminal-built STM32F103C8T6 implementation of the WEC NonceSense assignment.

## 1. Target and hardware

**MCU:** STM32F103C8T6 (Cortex-M3), Blue Pill style board  
**Clock:** internal HSI = 8 MHz; no PLL or vendor system file is used.

| Function | Pin / peripheral | Configuration |
|---|---|---|
| Status LED | PC13 | Active LOW, 2 MHz push-pull |
| User button | PB12 / EXTI12 | Input pull-up, pressed = LOW |
| UART TX | PA9 / USART1 | 115200 baud, 8-N-1 |
| UART RX | PA10 / USART1 | 115200 baud, 8-N-1 |
| Timer | TIM2 | 20 Hz (50 ms tick) in Task 3 |

Connect the USB-UART adapter **TX -> PA10 (RX)**, **RX -> PA9 (TX)** and **GND -> GND**. Do not connect a 5 V UART signal to the 3.3 V MCU pins.

The demo button should connect PB12 to GND when pressed. The internal pull-up keeps PB12 HIGH when released.

> The fixed `SECRET_KEY = 0x12345678` is intentionally public for this assignment. It demonstrates the protocol; it is not a production secret-storage design.

## 2. Repository layout

```text
NonceSense/
├── README.md
├── .gitignore
├── Task0/
│   ├── main.c
│   ├── startup.s
│   ├── linker.ld
│   └── Makefile
├── Task1/
│   ├── main.c
│   ├── startup.s
│   ├── linker.ld
│   ├── Makefile
│   └── host_test.py
├── Task2/
│   ├── main.c
│   ├── startup.s
│   ├── linker.ld
│   ├── Makefile
│   └── host_test.py
├── Task3/
│   ├── main.c
│   ├── startup.s
│   ├── linker.ld
│   ├── Makefile
│   └── host_test.py
└── docs/
    ├── STATE_MACHINE.md
    └── TEST_PLAN.md
```

Generated `build/` directories are intentionally ignored and are not part of the submission.

## 3. Toolchain and rules compliance

Each task is independently buildable with:

```text
arm-none-eabi-gcc
arm-none-eabi-objcopy
make
```

The Makefiles use:

```text
-Wall -Wextra -Werror -O2 -nostdlib -nostartfiles
```

and additionally use `-ffreestanding`, `-fno-common`, `-fno-builtin`, and section garbage collection.

No IDE project files are included. There are no `.ioc`, `.cproject`, `.uvprojx`, `platformio.ini`, CMSIS startup files, vendor startup files, vendor linker scripts, HAL, LL, Arduino APIs, or SDK APIs.

Task 0 intentionally includes only `<stdint.h>`.

Task 1–3 use direct STM32F1 register access as well; this keeps the submission self-contained and avoids a dependency on an external CMSIS/device package.

## 4. Build and flash

From a task directory:

```text
make
make clean
make flash
```

`make` creates:

```text
build/firmware.elf
build/firmware.bin
build/firmware.map
```

`make flash` expects `st-flash` to be installed and a supported ST-Link/debug probe to be connected. If using another flashing method, flash `build/firmware.bin` at `0x08000000`.

On Windows, run these commands from a shell where `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `make`, and (for `make flash`) `st-flash` are on `PATH`.

## 5. Boot flow: reset -> main()

The custom `startup.s` provides the complete vector table and reset handler.

1. On reset, the Cortex-M3 loads the initial stack pointer from address `0x08000000`.
2. It loads `Reset_Handler` from `0x08000004`.
3. `Reset_Handler` copies the initialized `.data` image from Flash to RAM.
4. It clears `.bss`.
5. It branches to `main()`.
6. Interrupt vectors point to the task's handlers; unused vectors resolve to `Default_Handler`.
7. The linker script places the vector table at the Flash origin and defines the RAM stack at `0x20005000`.

No C runtime startup code is linked.

## 6. Register map used

### RCC

Base: `0x40021000`

| Register | Offset | Address | Used bits |
|---|---:|---:|---|
| APB2ENR | 0x18 | `0x40021018` | IOPAEN bit 2, IOPBEN bit 3, IOPCEN bit 4, AFIOEN bit 0, USART1EN bit 14 |
| APB1ENR | 0x1C | `0x4002101C` | TIM2EN bit 0 |

### GPIO

| Peripheral | Base | Registers used |
|---|---:|---|
| GPIOA | `0x40010800` | CRH `+0x04` |
| GPIOB | `0x40010C00` | CRH `+0x04`, ODR `+0x0C` |
| GPIOC | `0x40011000` | CRH `+0x04`, ODR `+0x0C` |

### AFIO / EXTI

| Register | Address |
|---|---:|
| AFIO_EXTICR4 | `0x40010014` |
| EXTI_IMR | `0x40010400` |
| EXTI_RTSR | `0x40010408` |
| EXTI_FTSR | `0x4001040C` |
| EXTI_PR | `0x40010414` |

PB12 is selected as EXTI12 through AFIO_EXTICR4 field `[3:0] = 0001`.

### USART1

Base: `0x40013800`

| Register | Offset |
|---|---:|
| SR | 0x00 |
| DR | 0x04 |
| BRR | 0x08 |
| CR1 | 0x0C |

At 8 MHz PCLK2, `BRR = 0x45` gives 115200 baud.

Task 3 enables `CR1.RXNEIE` (bit 5). USART1 is IRQ 37, so NVIC ISER1 bit 5 is enabled.

### TIM2

Base: `0x40000000`

| Register | Offset |
|---|---:|
| CR1 | 0x00 |
| DIER | 0x0C |
| SR | 0x10 |
| EGR | 0x14 |
| PSC | 0x28 |
| ARR | 0x2C |

Task 3 uses `PSC=7999`, `ARR=49`:

```text
8 MHz / 8000 = 1 kHz
1 kHz / 50 = 20 Hz
20 Hz -> 50 ms per interrupt
```

TIM2 is IRQ 28, so NVIC ISER0 bit 28 is enabled.

### DWT cycle counter (Task 3)

| Register | Address |
|---|---:|
| DWT_CTRL | `0xE0001000` |
| DWT_CYCCNT | `0xE0001004` |
| CoreDebug DEMCR | `0xE000EDFC` |

Task 3 sets `DEMCR.TRCENA` and `DWT_CTRL.CYCCNTENA`, then measures only the constant-time comparison.

## 7. Task 1 protocol

```text
AUTH:A1B2C3D4
```

returns:

```text
RESP:346B02F5
```

for the assignment key `0x12345678`.

The response is:

```text
CRC32(challenge || key)
```

where both 32-bit values are serialized as four big-endian bytes before feeding the standard reflected CRC-32 algorithm.

## 8. Task 2 behavior

A valid `AUTH` puts the token into `WAIT_BUTTON`.

- LED toggles every 100 ms -> 5 Hz blink.
- TIM2 maintains the 10-second window.
- Button uses EXTI12.
- The button ISR only records `button_pressed`.
- CRC response computation happens in `main()` only after the button event.
- Timeout returns to standby and sends `ERR:TIMEOUT`.
- A command or button action in the wrong state produces `ERR:INVALID_STATE`.

## 9. Task 3 behavior

### UART ring buffer

USART1 receives bytes in `USART1_IRQHandler()` and stores them in a 128-byte circular buffer.

The ISR does not parse commands, calculate CRCs, print strings, or change the authentication state. The main loop pops bytes and assembles newline-terminated commands.

### VERIFY

```text
VERIFY:<8-HEX-CHARS>
```

The supplied response is compared with the expected response from the most recent successful button-authorized challenge.

The comparison:

```c
bool constant_time_memcmp(const uint8_t *a,
                          const uint8_t *b,
                          size_t len);
```

always visits every byte. It does not return on the first mismatch.

Task 3 prints:

```text
VERIFY_CYCLES:XXXXXXXX
OK
```

or:

```text
VERIFY_CYCLES:XXXXXXXX
FAIL
```

The host test extracts the cycle counts for a matching and non-matching value and reports whether they are identical.

### Anti-spam

A malformed command, invalid hexadecimal input, RX overflow, or complete command received during `WAIT_BUTTON` is a violation.

- Violations 1–3: UART input is discarded for 1 second.
- Violation 4+: 10-second lockout.
- During lockout all commands are ignored.
- Lockout LED toggles every 50 ms -> 10 Hz blink.
- Lockout prints exactly:

```text
[SECURITY ALERT] Malicious spam detected! Hardware locked for 10 seconds.
```

After the lockout, the violation counter resets.

