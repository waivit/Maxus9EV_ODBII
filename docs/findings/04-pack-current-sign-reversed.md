# 04 — Pack Current sign convention is reversed

- **Date:** 2026-07-16
- **Hypothesis under test:** H5 (new) — the sign convention assumed for Pack
  Current (`pids.py`: "Positive = charging, negative = discharging") is backwards.
  Also closes out live verification of H0 (uds.parse 11-bit fix).
- **Setup:** ELM327 v1.5 on COM7 (Bluetooth), vehicle READY, **not** plugged in /
  not charging (user-confirmed). `webdash.py` running locally, polled live via its
  `/data` endpoint on `127.0.0.1:8000` (read-only HTTP fetch, no direct serial
  access — avoids a second process fighting over the Bluetooth port).

## What I did
With the car sitting READY, took a baseline reading, then had the user increase
the AC blower load, then decrease it, capturing a live snapshot at each of the
3 states.

## Raw artifacts

| State | Pack Current raw | Value | 12V Battery | webdash label |
| --- | --- | --- | --- | --- |
| idle (baseline) | `9C 92` | +2.05 A | 14.00 V | CHARGING |
| AC blower increased | `9C EB` | **+4.28 A** | 13.00 V | CHARGING |
| AC blower decreased | `9C 64` | **+0.90 A** | 14.00 V | idle |

(Formula: `INT16 × 0.025 − 1000`, unchanged — only the sign's *meaning* is in
question here, not the decode.)

Also, across all 3 states every other PID decoded to sane, stable values matching
independently hand-computed formulas (Bus/Pack Voltage, Cell V max/min, temps) —
this is the live confirmation for H0 (see `03-uds-parser-11bit-header.md`).

## Observations
- The car was never plugged in / charging during this test — only idle vs. AC
  blower load changed.
- Pack Current magnitude **increased** when AC load increased, and **decreased**
  when AC load decreased — it tracks electrical **load**, not an external charger.
- `12V Battery` dropped from 14.00 V to 13.00 V under higher load, consistent with
  the DC-DC converter working harder — corroborating evidence of load-tracking,
  not charge-tracking.
- `webdash.py`'s `battery_state()` labelled all 3 states "CHARGING"/"idle" based on
  `current > 1.0 → CHARGING`, which is the wrong direction give this evidence.

## Interpretation
Positive Pack Current means the **pack is discharging** (supplying the HV bus to
run accessories/AC compressor), not charging. `src/pids.py`'s comment ("Positive =
charging, negative = discharging") and the `battery_state()` threshold logic in
both `src/diag.py` and `src/webdash.py` encode the **opposite** convention and
need to be flipped.

## Verdict
- **H5 → confirmed** (user-verified live, 2026-07-16, controlled AC-load experiment).
- **H0 → confirmed** (user-verified live, 2026-07-16): every PID decoded correctly
  across all 3 states, matching hand-computed formulas — the "no data" bug from
  the 6-char-header assumption is fixed. See `03-uds-parser-11bit-header.md` for
  the full root-cause record; this note supplies the live confirmation evidence.

## Next
**Fix applied 2026-07-16** (own iteration): flipped the `battery_state()`
charge/discharge branches in `diag.py` (reused by `webdash.py`), corrected the
`pids.py` comment, made the webdash state pill informational (neutral, not a
warning), and added `test_diag.py` as a regression guard for the mapping. Offline
regression green. Optional non-blocking sanity check next time at the car: the
dashboard should read "DISCHARGING" at idle instead of "CHARGING".
