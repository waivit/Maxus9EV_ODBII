"""
ELM327 Bluetooth Serial Interface
Handles low-level communication with ELM327 adapter.
"""

import serial
import serial.tools.list_ports
import time
import sys


class ELM327:
    def __init__(self, port: str, baud: int = 38400, timeout: int = 6):
        self.port    = port
        self.baud    = baud
        self.timeout = timeout
        self.ser     = None
        self._cur_header = None

    def connect(self) -> bool:
        try:
            self.ser = serial.Serial(
                port=self.port, baudrate=self.baud, timeout=self.timeout,
                parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                bytesize=serial.EIGHTBITS,
            )
            time.sleep(2)
            self.ser.flushInput()
            return True
        except serial.SerialException as e:
            print(f"Connect error: {e}")
            return False

    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def _send_raw(self, cmd: str, wait: float = 0.4) -> str:
        self.ser.flushInput()
        self.ser.write((cmd + "\r").encode())
        resp = b""
        start = time.time()
        while time.time() - start < self.timeout:
            if self.ser.in_waiting:
                resp += self.ser.read(self.ser.in_waiting)
                if b">" in resp:
                    break
            time.sleep(0.02)
        time.sleep(wait)
        return resp.decode("utf-8", errors="ignore").replace(">", "").strip()

    def send(self, cmd: str, wait: float = 0.4) -> str:
        return self._send_raw(cmd, wait)

    def set_header(self, header: str):
        if header != self._cur_header:
            self._send_raw(f"ATSH {header}", 0.2)
            self._cur_header = header

    def init(self) -> bool:
        print("Initializing ELM327...")
        for cmd, wait in [
            ("ATZ",    2.0),
            ("ATE0",   0.3),
            ("ATL0",   0.3),
            ("ATS0",   0.3),
            ("ATH1",   0.3),
            ("ATAT2",  0.3),
            ("ATST96", 0.3),
            ("ATSP6",  0.5),
        ]:
            resp = self._send_raw(cmd, wait)
            print(f"  {cmd}: {resp.replace(chr(13), ' ').strip()}")
        print("ELM327 ready.\n")
        return True


def select_com_port() -> str:
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No COM ports found.")
        sys.exit(1)
    print(f"\n{'No.':<5} {'Port':<10} {'Name'}")
    print("-" * 55)
    for i, p in enumerate(ports, 1):
        print(f"{i:<5} {p.device:<10} {(p.description or 'Unknown')[:40]}")
    print("-" * 55)
    while True:
        raw = input("\nSelect COM (number): ").strip()
        if not raw.isdigit():
            continue
        n = int(raw)
        if 1 <= n <= len(ports):
            return ports[n-1].device
        com = f"COM{n}"
        if any(p.device.upper() == com.upper() for p in ports):
            return com
