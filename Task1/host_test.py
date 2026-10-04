import sys
import time
import zlib

import serial

BAUD = 115200
SECRET_KEY = 0x12345678


def expected_response(challenge: int) -> str:
    data = challenge.to_bytes(4, "big") + SECRET_KEY.to_bytes(4, "big")
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else input("Serial port (e.g. COM5): ").strip()
    challenge = 0xA1B2C3D4

    with serial.Serial(port, BAUD, timeout=2) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        command = f"AUTH:{challenge:08X}"
        print(f">>> {command}")
        ser.write((command + "\n").encode("ascii"))

        response = ser.readline().decode("ascii", errors="replace").strip()
        print(f"<<< {response}")

        expected = f"RESP:{expected_response(challenge)}"
        if response != expected:
            raise SystemExit(f"FAIL: expected {expected!r}")

        print("PASS: Task 1 challenge-response matches the host CRC32 calculation.")


if __name__ == "__main__":
    main()
