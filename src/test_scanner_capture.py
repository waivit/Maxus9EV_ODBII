"""
Regression guard for scanner.py's raw-capture persistence (save_capture /
scan_range capture_path). No hardware needed — the ELM327 serial link is faked.

SCOPE: checks only that captures are written correctly to disk (one JSONL line
per attempted PID, positive and negative both present, correct fields). This is
NOT a vehicle test — it doesn't validate what any PID actually means.
Run: python test_scanner_capture.py
"""

import os
import json
import tempfile

from scanner import scan_range

failures = 0


def check(name, cond):
    global failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        failures += 1


class FakeSerial:
    """Feeds canned raw ELM327 responses for a small B0xx range: one positive,
    one negative (NRC 0x31), one no-data ('NO DATA' style garbage)."""

    RESPONSES = {
        "22B040": b"7ED0562B040065E>",       # positive, data 06 5E
        "22B041": b"7ED037F2231>",           # NRC 0x31
        "22B042": b"NODATA>",                # unparseable / no data
    }

    def __init__(self):
        self._next = b""

    def flushInput(self):
        pass

    def write(self, data: bytes):
        cmd = data.decode().strip().replace(" ", "")
        self._next = self.RESPONSES.get(cmd, b"NODATA>")

    @property
    def in_waiting(self):
        return len(self._next)

    def read(self, n):
        out, self._next = self._next[:n], self._next[n:]
        return out


class FakeELM:
    def __init__(self):
        self.ser = FakeSerial()

    def set_header(self, h):
        pass


with tempfile.TemporaryDirectory() as tmp:
    cap_path = os.path.join(tmp, "test_capture.jsonl")
    elm = FakeELM()

    found = scan_range(elm, "7E5", "B0", lo=0x40, hi=0x42, capture_path=cap_path)

    print("== scan_range return value ==")
    check("found 1 positive PID (B040)", found == [("B040", [0x06, 0x5E])])

    print("== capture file contents ==")
    check("capture file exists", os.path.exists(cap_path))
    with open(cap_path, encoding="utf-8") as f:
        lines = [json.loads(l) for l in f if l.strip()]

    check("3 lines written (one per attempted PID)", len(lines) == 3)

    by_pid = {e["pid"]: e for e in lines}
    check("B040 present and positive", by_pid.get("B040", {}).get("positive") is True)
    check("B040 data == [6, 94]", by_pid.get("B040", {}).get("data") == [6, 94])
    check("B041 present and NOT positive (NRC)", by_pid.get("B041", {}).get("positive") is False)
    check("B042 present and NOT positive (no data)", by_pid.get("B042", {}).get("positive") is False)
    check("every entry has header/cmd/raw fields",
          all({"header", "cmd", "raw", "data_hex", "nrc_name"} <= e.keys() for e in lines))

print()
if failures:
    print(f"REGRESSION: {failures} FAILED")
    raise SystemExit(1)
print("REGRESSION: OK (capture file-writing matches expectations; NOT a vehicle test)")
