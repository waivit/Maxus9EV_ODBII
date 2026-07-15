"""
MAXUS 9 EV - Battery Data Reader
Reads confirmed PIDs and displays live data.
"""

import time
import sys
from elm327 import ELM327, select_com_port
from uds import parse
from pids import CONFIRMED, UNDER_INVESTIGATION


def read_pid(elm: ELM327, pid) -> float | None:
    elm.set_header(pid.header)
    resp = elm.send(pid.command, 0.5)
    raw  = resp.upper().replace(" ","").replace("\r","").replace("\n","")
    p    = parse(raw)
    if p["positive"] and p["data"]:
        try:
            return pid.formula(p["data"])
        except Exception:
            return None
    return None


def read_all(elm: ELM327, pids: list, verbose: bool = True) -> dict:
    results = {}
    for pid in pids:
        val = read_pid(elm, pid)
        results[pid.name] = val
        if verbose:
            val_str = f"{val:.2f} {pid.unit}" if val is not None else "no data"
            print(f"  {pid.name:<25} {val_str}")
    return results


def live_loop(elm: ELM327, pids: list, interval: float = 2.0):
    print("\nLive data (Ctrl+C to stop):\n")
    while True:
        try:
            parts = []
            for pid in pids:
                val = read_pid(elm, pid)
                if val is not None:
                    parts.append(f"{pid.name}: {val:.1f}{pid.unit}")
                else:
                    parts.append(f"{pid.name}: --")
            print(f"\r  {'  |  '.join(parts):<100}", end="", flush=True)
            time.sleep(interval)
        except KeyboardInterrupt:
            print("\n\nStopped.")
            break


def main():
    print("=" * 55)
    print(" MAXUS 9 EV - Battery Reader")
    print("=" * 55)

    port = select_com_port()
    elm  = ELM327(port)

    if not elm.connect():
        sys.exit(1)

    elm.init()

    print("=" * 55)
    print("Confirmed PIDs:")
    print("=" * 55)
    results = read_all(elm, CONFIRMED)

    print("\n" + "=" * 55)
    print("Under Investigation:")
    print("=" * 55)
    read_all(elm, UNDER_INVESTIGATION)

    ans = input("\nStart live read? (y/n): ").strip().lower()
    if ans == "y":
        live_loop(elm, CONFIRMED)

    elm.disconnect()


if __name__ == "__main__":
    main()
