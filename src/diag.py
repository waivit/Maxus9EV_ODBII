"""
MAXUS 9 EV - Battery Diagnostic Dashboard
Reads confirmed PIDs, evaluates health thresholds, and flags OK / WARN / ALARM.

Two modes:
  1. snapshot - read once, print a table with alert status, exit
  2. live     - loop and refresh the screen (Ctrl+C to stop)

Reuses the existing infrastructure (ELM327, UDS parser, confirmed PIDs).
Scope: dashboard + alerts only. No CSV logging, no new PIDs in pids.py.
Cell imbalance and temp spread are computed here (for alerts only).
"""

import os
import sys
import time
from elm327 import ELM327, select_com_port
from uds import parse
from pids import CONFIRMED


# ======================================================
# Alert thresholds  (kind: "high" = big is bad, "low" = small is bad)
# Tune these once the MAXUS 9 cell spec is confirmed.
# ======================================================
THRESHOLDS = {
    # key                (kind,   warn,  alarm,  unit)
    "Cell Imbalance":    ("high",  30.0,  50.0,  "mV"),
    "Batt Temp High":    ("high",  40.0,  48.0,  "C"),
    "Temp Spread":       ("high",   5.0,   8.0,  "C"),
    "Cell Voltage Min":  ("low",    3.3,   3.1,  "V"),
    "Cell Voltage Max":  ("high",  4.15,   4.2,  "V"),
    "12V Battery":       ("low",   12.2,  11.8,  "V"),
}

# Status tags. ANSI colour is enabled on Windows via os.system("") in main().
TAG = {
    "OK":    "\033[32m[ OK ]\033[0m",
    "WARN":  "\033[33m[WARN]\033[0m",
    "ALARM": "\033[31m[ALRM]\033[0m",
    "--":    "[ -- ]",
}


def read_pid(elm: ELM327, pid) -> float | None:
    """Same pattern as reader.py:read_pid."""
    elm.set_header(pid.header)
    resp = elm.send(pid.command, 0.5)
    raw  = resp.upper().replace(" ", "").replace("\r", "").replace("\n", "")
    p    = parse(raw)
    if p["positive"] and p["data"]:
        try:
            return pid.formula(p["data"])
        except Exception:
            return None
    return None


def evaluate(key: str, value: float | None) -> str:
    """Return OK / WARN / ALARM / -- for a value against THRESHOLDS[key]."""
    if value is None:
        return "--"
    spec = THRESHOLDS.get(key)
    if spec is None:
        return "OK"
    kind, warn, alarm, _unit = spec
    if kind == "high":
        if value >= alarm:
            return "ALARM"
        if value >= warn:
            return "WARN"
        return "OK"
    else:  # "low" - small is bad
        if value <= alarm:
            return "ALARM"
        if value <= warn:
            return "WARN"
        return "OK"


def collect(elm: ELM327) -> dict:
    """Read all confirmed PIDs, then add computed imbalance / temp spread."""
    values = {pid.name: read_pid(elm, pid) for pid in CONFIRMED}

    cmax, cmin = values.get("Cell Voltage Max"), values.get("Cell Voltage Min")
    if cmax is not None and cmin is not None:
        values["Cell Imbalance"] = (cmax - cmin) * 1000.0  # V -> mV
    else:
        values["Cell Imbalance"] = None

    thi, tlo = values.get("Batt Temp High"), values.get("Batt Temp Low")
    if thi is not None and tlo is not None:
        values["Temp Spread"] = thi - tlo
    else:
        values["Temp Spread"] = None

    return values


def battery_state(pack_current: float | None) -> str:
    """Derive charge/discharge/idle from pack current sign (+ = charging)."""
    if pack_current is None:
        return "unknown"
    if pack_current > 1.0:
        return "CHARGING"
    if pack_current < -1.0:
        return "DISCHARGING"
    return "idle"


# Display order and units for the dashboard table.
ROWS = [
    ("Battery State",     ""),
    ("Pack Voltage",      "V"),
    ("Pack Current",      "A"),
    ("Bus Voltage",       "V"),
    ("Cell Voltage Max",  "V"),
    ("Cell Voltage Min",  "V"),
    ("Cell Imbalance",    "mV"),
    ("Batt Temp High",    "C"),
    ("Batt Temp Low",     "C"),
    ("Temp Spread",       "C"),
    ("12V Battery",       "V"),
    ("Charge Status",     ""),
]


def render(values: dict) -> str:
    """Build the dashboard table as a string."""
    state = battery_state(values.get("Pack Current"))
    lines = []
    worst = "OK"
    order = {"OK": 0, "WARN": 1, "ALARM": 2, "--": 0}

    for name, unit in ROWS:
        if name == "Battery State":
            lines.append(f"  {name:<20} {state:>10}       {TAG['OK']}")
            continue

        val = values.get(name)
        status = evaluate(name, val)
        if order[status] > order[worst]:
            worst = status

        if val is None:
            val_str = "no data"
        elif name in ("Cell Imbalance",):
            val_str = f"{val:.0f} {unit}"
        elif name in ("Charge Status",):
            val_str = f"{val:.0f}"
        else:
            val_str = f"{val:.2f} {unit}"

        lines.append(f"  {name:<20} {val_str:>13}   {TAG[status]}")

    header = "=" * 46
    summary = f"  OVERALL: {TAG[worst]}"
    return "\n".join([header, *lines, header, summary])


def snapshot(elm: ELM327):
    print("\nReading snapshot...\n")
    values = collect(elm)
    print(render(values))
    print()


def live(elm: ELM327, interval: float = 2.0):
    print("\nLive dashboard (Ctrl+C to stop)...")
    time.sleep(0.5)
    try:
        while True:
            values = collect(elm)
            os.system("cls" if os.name == "nt" else "clear")
            print(" MAXUS 9 EV - Battery Diagnostic (live)")
            print(render(values))
            print(f"\n  refresh {interval:.0f}s - Ctrl+C to stop", flush=True)
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n\nStopped.")


def main():
    os.system("")  # enable ANSI colour on Windows terminals (Win10+)

    print("=" * 46)
    print(" MAXUS 9 EV - Battery Diagnostic")
    print("=" * 46)

    port = select_com_port()
    elm  = ELM327(port)
    if not elm.connect():
        sys.exit(1)
    elm.init()

    print("Mode:")
    print("  1 - Snapshot (read once)")
    print("  2 - Live dashboard")
    choice = input("Choice [1]: ").strip() or "1"

    if choice == "2":
        live(elm)
    else:
        snapshot(elm)

    elm.disconnect()


if __name__ == "__main__":
    main()
