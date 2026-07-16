"""
MAXUS 9 EV - PID Scanner
Scan unknown PID ranges and compare snapshots.
Used for discovering new PIDs (e.g. SOC, regen level).
"""

import time
import sys
import os
import json
from datetime import datetime
from elm327 import ELM327, select_com_port
from uds import parse

CAPTURES_DIR = os.path.join(os.path.dirname(__file__), "..", "captures")


def save_capture(entries: list, path: str):
    """Append raw scan entries (list of dict) to a JSONL capture file."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        for entry in entries:
            f.write(json.dumps(entry) + "\n")


def scan_range(elm: ELM327, header: str, prefix: str,
               lo: int = 0x00, hi: int = 0xFF, capture_path: str = None) -> list:
    """
    Scan Service 22 PIDs prefix+lo to prefix+hi on given header.
    Returns list of (pid_hex, data_bytes) for positive responses.

    If capture_path is given, every attempted PID (positive AND negative/NRC) is
    appended to that JSONL file immediately, so results survive a Ctrl+C and can
    be re-analysed offline later without returning to the vehicle.
    """
    print(f"\nScanning {header} 22 {prefix}{lo:02X}-{prefix}{hi:02X}...")
    elm.set_header(header)
    found = []

    for b in range(lo, hi + 1):
        cmd = f"22 {prefix[0:2]} {b:02X}"
        elm.ser.flushInput()
        elm.ser.write((cmd + "\r").encode())

        resp_raw = b""
        start = time.time()
        while time.time() - start < 1.2:
            if elm.ser.in_waiting:
                resp_raw += elm.ser.read(elm.ser.in_waiting)
                if b">" in resp_raw:
                    break
            time.sleep(0.02)

        resp = resp_raw.decode("utf-8", errors="ignore").replace(">","").strip()
        raw  = resp.upper().replace(" ","").replace("\r","").replace("\n","")
        p    = parse(raw)

        if capture_path:
            save_capture([{
                "header":   header,
                "pid":      f"{prefix}{b:02X}",
                "cmd":      cmd,
                "raw":      raw,
                "positive": p["positive"],
                "data":     p["data"],
                "data_hex": p["data_hex"],
                "nrc_name": p["nrc_name"],
            }], capture_path)

        if p["positive"]:
            hex_d = " ".join(f"{x:02X}" for x in p["data"])
            dec_d = " ".join(str(x) for x in p["data"])
            print(f"  FOUND {prefix}{b:02X}: hex=[{hex_d}]  dec=[{dec_d}]")
            found.append((f"{prefix}{b:02X}", p["data"]))

        print(f"\r  {b-lo+1}/{hi-lo+1}", end="", flush=True)

    print(f"\n  Done. Found {len(found)} PIDs.")
    if capture_path:
        print(f"  Raw capture saved: {capture_path}")
    return found


def snapshot(elm: ELM327, pids: list) -> dict:
    """
    Read a list of (header, command) tuples.
    Returns {header:command: data_bytes}.
    """
    results = {}
    for header, cmd in pids:
        elm.set_header(header)
        resp = elm.send(cmd, 0.4)
        raw  = resp.upper().replace(" ","").replace("\r","").replace("\n","")
        p    = parse(raw)
        if p["positive"]:
            results[f"{header}:{cmd}"] = p["data"]
    return results


def diff(before: dict, after: dict) -> list:
    """Show keys where data changed."""
    changed = []
    for key in sorted(set(before) | set(after)):
        b = before.get(key, [])
        a = after.get(key, [])
        if b != a:
            header, cmd = key.split(":", 1)
            b_hex = " ".join(f"{x:02X}" for x in b)
            a_hex = " ".join(f"{x:02X}" for x in a)
            print(f"  {header} {cmd}: [{b_hex}] -> [{a_hex}]")
            changed.append((header, cmd, b, a))
    if not changed:
        print("  No changes.")
    return changed


if __name__ == "__main__":
    print("=" * 55)
    print(" MAXUS 9 EV - PID Scanner")
    print("=" * 55)

    port = select_com_port()
    elm  = ELM327(port)
    if not elm.connect():
        sys.exit(1)
    elm.init()

    print("Scan options:")
    print("  1 - Scan B0xx on 7E5 (BMS range)")
    print("  2 - Scan B4xx on 7E3 (VCU range)")
    print("  3 - Custom range")
    choice = input("Choice: ").strip()

    def capture_path_for(header: str, prefix: str) -> str:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        return os.path.join(CAPTURES_DIR, f"{ts}_{header}_{prefix}.jsonl")

    if choice == "1":
        scan_range(elm, "7E5", "B0", capture_path=capture_path_for("7E5", "B0"))
    elif choice == "2":
        scan_range(elm, "7E3", "B4", capture_path=capture_path_for("7E3", "B4"))
    elif choice == "3":
        hdr = input("Header (e.g. 7E5): ").strip()
        pfx = input("Prefix (e.g. B0): ").strip()
        lo  = int(input("From (hex, e.g. 00): ").strip(), 16)
        hi  = int(input("To   (hex, e.g. FF): ").strip(), 16)
        scan_range(elm, hdr, pfx, lo, hi, capture_path=capture_path_for(hdr, pfx))

    elm.disconnect()
