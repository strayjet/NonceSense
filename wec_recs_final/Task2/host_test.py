import sys
import time

import serial

BAUD = 115200


def read_line(ser, timeout=2.0):
    old = ser.timeout
    ser.timeout = timeout
    try:
        return ser.readline().decode("ascii", errors="replace").strip()
    finally:
        ser.timeout = old


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else input("Serial port (e.g. COM5): ").strip()

    with serial.Serial(port, BAUD, timeout=2) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        print(">>> AUTH:A1B2C3D4")
        ser.write(b"AUTH:A1B2C3D4\n")
        line = read_line(ser)
        print(f"<<< {line}")

        if line != "WAIT:PRESS_BUTTON":
            raise SystemExit("FAIL: token did not enter WAIT_BUTTON")

        input("Press the physical PB12 button, then press ENTER here...")
        deadline = time.time() + 2.0
        response = ""
        while time.time() < deadline:
            line = read_line(ser, 0.2)
            if line:
                print(f"<<< {line}")
                if line.startswith("RESP:"):
                    response = line
                    break

        if not response:
            raise SystemExit("FAIL: no response after button press")

        print(f"PASS: response emitted only after button event: {response}")

        # Timeout test.
        print(">>> AUTH:01020304")
        ser.write(b"AUTH:01020304\n")
        line = read_line(ser)
        print(f"<<< {line}")

        if line != "WAIT:PRESS_BUTTON":
            raise SystemExit("FAIL: second AUTH did not start")

        print("Waiting 10.5 seconds for timeout...")
        deadline = time.time() + 11.0
        timeout_seen = False
        while time.time() < deadline:
            line = read_line(ser, 0.5)
            if line:
                print(f"<<< {line}")
                if line == "ERR:TIMEOUT":
                    timeout_seen = True
                    break

        if not timeout_seen:
            raise SystemExit("FAIL: timeout was not reported")

        print("PASS: Task 2 user-presence and timeout tests completed.")


if __name__ == "__main__":
    main()
