# NonceSense Task 2/3 State Machine

```text
                         valid AUTH
             +-------------------------------+
             |                               v
             |                          +-----------+
             |                          | WAIT_BTN  |
             |                          +-----------+
             |                           |   |    |
             |                button ----+   |    +---- 10 s ----+
             |                               |                 |
             v                               v                 v
        +-----------+                    +-----------+      +-----------+
        |  STANDBY  |<-------------------|  STANDBY  |      |  STANDBY  |
        +-----------+                    +-----------+      +-----------+

Task 3 security states:

 STANDBY -- violation 1..3 --> DISCARD --1 s--> STANDBY
    |
    +-- violation 4+ -------> LOCKOUT --10 s--> STANDBY

WAIT_BUTTON -- complete command --> violation --> DISCARD/LOCKOUT
LOCKOUT     -- any UART input --> ignored
DISCARD     -- any UART input --> discarded
```

## Event responsibilities

- **Button ISR:** clear EXTI pending bit and set `button_pressed`.
- **TIM2 ISR:** maintain timing and LED indication.
- **USART1 ISR (Task 3):** move RX bytes into the ring buffer.
- **Main loop:** parse commands, perform CRC work, perform constant-time verification, and transition protocol state.

The separation prevents long protocol work from running inside the external-interrupt or UART receive handlers.
