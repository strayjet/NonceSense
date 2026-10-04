# NonceSense hardware test plan

These are procedures to execute on the real board. The repository intentionally contains no fabricated hardware logs.

## Task 0

1. Flash `Task0/build/firmware.bin`.
2. Confirm PC13 LED toggles continuously at approximately 1 Hz.

## Task 1

1. Connect USB-UART at 115200 8-N-1.
2. Flash Task 1.
3. Send `AUTH:A1B2C3D4`.
4. Expect `RESP:346B02F5`.
5. Repeat the same command and confirm the LED toggles on every successful response.
6. Try malformed and invalid-hex commands.

## Task 2

1. Send `AUTH:A1B2C3D4`.
2. Confirm `WAIT:PRESS_BUTTON`.
3. Confirm LED blink is 5 Hz.
4. Press PB12 within 10 seconds.
5. Confirm a `RESP:` line is emitted only after the button event.
6. Repeat without pressing the button and confirm `ERR:TIMEOUT`.
7. Press the button while in standby and confirm `ERR:INVALID_STATE`.

## Task 3

1. Send `AUTH:A1B2C3D4`.
2. Press the button.
3. Record the `RESP:` value.
4. Send `VERIFY:<recorded response>` and confirm `OK`.
5. Send a wrong response such as `VERIFY:00000000` and confirm `FAIL`.
6. Repeat matching and non-matching verification and record `VERIFY_CYCLES`.
7. Confirm matching and non-matching comparison cycle counts are equal when no unrelated interrupt occurs during the measurement.
8. Trigger violations 1–3 and confirm each produces `ERR:INPUT_DISCARDED_1S`.
9. Trigger violation 4 and confirm the exact security-alert string and 10-second lockout.
10. During lockout, send commands and confirm there is no command response.
11. After 10 seconds, confirm the token returns to standby.

## Host automation

`Task1/host_test.py`, `Task2/host_test.py`, and `Task3/host_test.py` automate the serial portions and print the observed protocol responses.
