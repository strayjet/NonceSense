# NonceSense command reference

All commands are ASCII and newline terminated.

| Command | Valid state | Success | Invalid input |
|---|---|---|---|
| `AUTH:<8 hex>` | STANDBY | `WAIT:PRESS_BUTTON` | violation |
| `VERIFY:<8 hex>` | STANDBY | `OK` / `FAIL` | invalid hex = violation |
| any command | WAIT_BUTTON | — | violation / active-operation spam |
| any command | DISCARD | ignored | ignored |
| any command | LOCKOUT | ignored | ignored |

## Example

```text
AUTH:A1B2C3D4
WAIT:PRESS_BUTTON
RESP:346B02F5
VERIFY:346B02F5
VERIFY_CYCLES:000000XX
OK
VERIFY:00000000
VERIFY_CYCLES:000000XX
FAIL
```

The exact cycle count is hardware/compiler dependent. The required property is that the measured count for the matching and non-matching comparison is identical under otherwise identical conditions.
