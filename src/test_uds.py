"""
Regression guard for uds.parse against real 11-bit CAN responses captured from
the MAXUS 9 EV (ELM327 v1.5, COM7, 2026-07-16). No hardware needed.

SCOPE: this only checks that the decode logic still matches these known captured
bytes (guards against future breakage). It is built from the same samples the
parser was written against, so it is NOT verification of the hypothesis and NOT a
substitute for a live test at the vehicle — that is where pass/fail is really
decided. See docs/findings/03-uds-parser-11bit-header.md.

Run: python test_uds.py
"""

from uds import parse

# (raw response, expected did, expected data bytes) — real single-frame captures.
POSITIVE = [
    ("7ED0562B041065E", "B041", [0x06, 0x5E]),   # Bus Voltage   -> 407.5 V
    ("7ED0562B042065F", "B042", [0x06, 0x5F]),   # Pack Voltage  -> 407.75 V
    ("7ED0562B0439C90", "B043", [0x9C, 0x90]),   # Pack Current
    ("7ED0562B0568F06", "B056", [0x8F, 0x06]),   # Batt Temp High
    ("7ED0662B0580EC20B", "B058", [0x0E, 0xC2, 0x0B]),  # Cell V Max (3 bytes)
    ("7ED0662B0590EB725", "B059", [0x0E, 0xB7, 0x25]),  # Cell V Min (3 bytes)
    ("7EB046201120E", "0112", [0x0E]),           # 12V Battery on VCU (7EB)
    ("7ED0462B04C03", "B04C", [0x03]),           # was mislabelled SOH
]

NEGATIVE = [
    ("7ED037F2231", 0x31),                       # requestOutOfRange
]

failures = 0


def check(name, cond):
    global failures
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        failures += 1


print("== positive responses ==")
for raw, did, data in POSITIVE:
    r = parse(raw)
    check(f"{raw} positive", r["positive"])
    check(f"{raw} did == {did}", r["did"] == did)
    check(f"{raw} data == {data}", r["data"] == data)

print("== negative responses (NRC) ==")
for raw, nrc in NEGATIVE:
    r = parse(raw)
    check(f"{raw} not positive", not r["positive"])
    check(f"{raw} nrc_code == 0x{nrc:02X}", r["nrc_code"] == nrc)

print("== physical sanity (decoded values) ==")
bus = parse("7ED0562B041065E")["data"]
check("Bus Voltage ~= 407.5 V", abs(((bus[0] * 256 + bus[1]) * 0.25) - 407.5) < 0.01)
cmax = parse("7ED0662B0580EC20B")["data"]
check("Cell V Max ~= 3.778 V", abs(((cmax[0] * 256 + cmax[1]) * 0.001) - 3.778) < 0.001)

print()
if failures:
    print(f"REGRESSION: {failures} FAILED")
    raise SystemExit(1)
print("REGRESSION: OK (decode matches captured samples; NOT live verification)")
