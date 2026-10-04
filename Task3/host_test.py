import re
import sys
import time

import serial

BAUD = 115200
CYCLES_RE = re.compile(r"^VERIFY_CYCLES:([0-9A-Fa-f]{8})$")


def read_available(ser, seconds=1.0):
    end = time.time() + seconds
    lines = []
    while time.time() < end:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if line:
            print(f"<<< {line}")
            lines.append(line)
        else:
            time.sleep(0.02)
    return lines


def send(ser, command):
    print(f">>> {command}")
    ser.write((command + "\n").encode("ascii"))


def wait_for(ser, prefix, timeout=2.0):
    end = time.time() + timeout
    while time.time() < end:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if line:
            print(f"<<< {line}")
            if line.startswith(prefix):
                return line
    raise RuntimeError(f"Timed out waiting for {prefix!r}")


def wait_exact(ser, expected, timeout=2.0):
    end = time.time() + timeout
    while time.time() < end:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if line:
            print(f"<<< {line}")
            if line == expected:
                return
    raise RuntimeError(f"Timed out waiting for {expected!r}")


def verify(ser, value):
    send(ser, f"VERIFY:{value}")
    cycle_line = wait_for(ser, "VERIFY_CYCLES:")
    result = wait_for(ser, "")
    match = CYCLES_RE.fullmatch(cycle_line)
    if not match:
        raise RuntimeError(f"bad cycle line: {cycle_line}")
    return int(match.group(1), 16), result


def cause_violation(ser, index):
    send(ser, f"BAD_COMMAND_{index}")
    wait_exact(ser, "ERR:INPUT_DISCARDED_1S")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else input("Serial port (e.g. COM5): ").strip()

    with serial.Serial(port, BAUD, timeout=0.5) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        # User-presence authentication.
        send(ser, "AUTH:A1B2C3D4")
        wait_exact(ser, "WAIT:PRESS_BUTTON")

        input("Press PB12, then press ENTER here...")
        response_line = wait_for(ser, "RESP:")
        expected = response_line.split(":", 1)[1]
        print(f"Captured response: {expected}")

        # Constant-time comparison test.
        match_cycles, match_result = verify(ser, expected)
        if match_result != "OK":
            raise SystemExit("FAIL: correct VERIFY did not return OK")

        wrong = "00000000" if expected != "00000000" else "FFFFFFFF"
        mismatch_cycles, mismatch_result = verify(ser, wrong)
        if mismatch_result != "FAIL":
            raise SystemExit("FAIL: incorrect VERIFY did not return FAIL")

        print(f"Matching cycles:     {match_cycles}")
        print(f"Non-matching cycles: {mismatch_cycles}")
        if match_cycles != mismatch_cycles:
            raise SystemExit("FAIL: measured comparison cycles differ")

        print("PASS: constant-time comparison cycle counts match.")

        # Violations 1-3.
        for i in range(1, 4):
            cause_violation(ser, i)
            time.sleep(1.15)

        # Violation 4 -> lockout.
        send(ser, "BAD_COMMAND_4")
        wait_exact(
            ser,
            "[SECURITY ALERT] Malicious spam detected! "
            "Hardware locked for 10 seconds.",
        )
        print("PASS: fourth violation entered 10-second lockout.")

        # Commands must be ignored during lockout.
        time.sleep(1.0)
        send(ser, "AUTH:11223344")
        time.sleep(0.5)
        lines = read_available(ser, 0.5)
        if any("WAIT:" in line or "RESP:" in line for line in lines):
            raise SystemExit("FAIL: command was accepted during lockout")

        print("Waiting for lockout to expire...")
        time.sleep(9.5)

        send(ser, "AUTH:11223344")
        wait_exact(ser, "WAIT:PRESS_BUTTON")

        print("PASS: Task 3 anti-spam and lockout tests completed.")


if __name__ == "__main__":
    main()
