"""
Regression guard for diag.py's battery_state() sign convention and evaluate()
alert logic. No hardware needed (runs in the venv where pyserial is present).

SCOPE: checks the code matches the confirmed Pack Current sign convention (H5,
docs/findings/04-pack-current-sign-reversed.md) and the alert thresholds. This is
a regression guard, NOT a vehicle test.
Run: python test_diag.py
"""

from diag import battery_state, evaluate

failures = 0


def check(name, cond):
    global failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        failures += 1


print("== battery_state() sign convention (H5: + = discharging) ==")
check("positive current -> DISCHARGING", battery_state(50.0) == "DISCHARGING")
check("negative current -> CHARGING", battery_state(-50.0) == "CHARGING")
check("near-zero -> idle", battery_state(0.0) == "idle")
check("None -> unknown", battery_state(None) == "unknown")
# boundary: |current| <= 1.0 A counts as idle
check("+0.5 A -> idle", battery_state(0.5) == "idle")
check("-0.5 A -> idle", battery_state(-0.5) == "idle")

print("== evaluate() alert logic (shared in diag.py) ==")
check("Cell Imbalance 12 -> OK", evaluate("Cell Imbalance", 12.0) == "OK")
check("Cell Imbalance 40 -> WARN", evaluate("Cell Imbalance", 40.0) == "WARN")
check("Cell Imbalance 70 -> ALARM", evaluate("Cell Imbalance", 70.0) == "ALARM")
check("Cell Voltage Min 3.0 -> ALARM", evaluate("Cell Voltage Min", 3.0) == "ALARM")
check("None -> --", evaluate("Cell Voltage Min", None) == "--")

print()
if failures:
    print(f"REGRESSION: {failures} FAILED")
    raise SystemExit(1)
print("REGRESSION: OK (sign + alert logic match spec; NOT a vehicle test)")
