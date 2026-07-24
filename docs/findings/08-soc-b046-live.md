# 08 — SOC B046 vs dash (first live verification)

- **Date:** 2026-07-17
- **Hypothesis under test:** H1 (SOC = `B046`, 7E5)
- **Setup:** ESP32 bridge (poc3) reading `22 B0 46` on header 7E5 over BT, value
  shown on the phone dashboard as `INT16 × 0.1`. Compared against the car's own
  dash SOC %, at two states: before charge, and after charging up.

## What I did
Read the mobile SOC (our `B046 ÷ 10`) and the dash SOC at the same moment, once
low and once after charging.

## Raw artifacts
Mobile value = `INT16 × 0.1`, so `INT16 = mobile × 10`:

| State | Dash SOC | Mobile (B046÷10) | ⇒ B046 INT16 |
| --- | --- | --- | --- |
| before charge | 49 % | 45.2 | 452 |
| after charge  | 80 % | 57.2 | 572 |

Prior reference (note 07, one point): `B046 = 022D = 557` at dash ~50–55 %.

## Observations
- `÷10` error is **not constant**: −3.8 at 49 %, −22.8 at 80 % (grows with SOC).
- The three (INT16, dash) points are **not collinear**:
  - (452, 49) → (572, 80): slope 0.258 %/unit.
  - (452, 49) → (557, ~52): slope ~0.033.
  - (557, ~52) → (572, 80): slope ~1.83.
  A 2-point linear fit of the two new points (`dash ≈ 0.258·INT16 − 67.8`, i.e.
  MG-form `lo≈262, hi≈650`) predicts **76 %** at INT16 557 — but the dash was
  ~52 % there.

## Interpretation
`B046 ÷ 10` is **wrong**, and no single linear map fits all three points, so
`B046` does **not** linearly track the *displayed* dash SOC. Most likely `B046` is
an **internal/BMS SOC** (raw cell-balance or coulomb-count state) that differs
from the buffered value the dash shows, or the dash SOC comes from a **different
PID** altogether. Refitting `lo/hi` from two points would be false precision and is
contradicted by the third point.

## Verdict
- **H1: `÷10` formula REJECTED.** SOC PID/formula **unresolved** — H1 stays
  `testing` but "current formula rejected; B046 may not be dash SOC".
- Do **not** apply an unvalidated refit to the dashboard; SOC stays flagged
  `test`/contradicted until the real source is identified.
- `00-hypotheses.md` H1 updated accordingly.

---

## Follow-up (2026-07-17) — after cross-referencing 4 sources → conclusion corrected

The "Interpretation/Verdict" above was written from our data alone. A 4-source
cross-reference (`09-ovms-cross-reference.md`) **corrects it**:

- **`B046 ÷ 10` IS the SOC** — specifically the **"Raw SOC"**. OVMS firmware, the
  mgevs community RE, *and* the Torque Pro CSV all agree (the Torque CSV names the
  fields `Raw_SOC` and `Display_SOC` explicitly). So the earlier wording "B046 may
  not be dash SOC / SOC PID unresolved" was **too pessimistic**.
- **The dash shows a derived value.** Torque computes
  `Display_SOC = 1.1 × (Raw_SOC + BMS_Version_SoC_Factor)`, clamped 0–100, and its
  README states the correction **depends on the BMS software version**.
- ⇒ We had **the right PID with a missing transform**, not the wrong PID. What is
  actually unresolved is the **per-vehicle/BMS-version calibration**.

**Our data still doesn't fit even that shape** (implied gain 1.08 @49 % vs 1.40
@80 %), which now points at the **80 % sample being stale** — Raw SOC read right
after charging had likely not updated yet. That single point drove the original
"non-collinear" conclusion, so it must be re-measured before drawing any curve.

**Revised verdict:** H1 = `testing`. `B046÷10` = Raw SOC (ref-confirmed);
Display-SOC transform **uncalibrated for MAXUS**. `005B` (`data[0]×100/255`) is a
weak secondary lead only — the MG community does not use it for the dash SOC.

## Next
Primary: **multi-point calibration** — read Raw `B046` against the dash at ~30/50/
70/90 %, with the car **settled** (not immediately after charging), and re-take the
80 % point to test the staleness theory. Fit `Display = gain × (Raw + factor)`.
Read `005B` opportunistically in the same session.

Secondary (only if the above fails): identify a different PID that tracks the
**dash** SOC, using the existing
`src/scanner.py` (`snapshot()` + `diff()`):
1. Full `B0xx` scan on 7E5 (menu option 1, raw JSONL) at a **known** dash SOC.
2. Repeat after a large SOC change; `diff` the two captures.
3. The PID(s) whose value moves with the dash are the SOC candidates.
Fast heuristic: dash `49 = 0x31`, dash `80 = 0x50` — look for a byte reading
`0x31 → 0x50` (SOC ×1) or `0x62 → 0xA0` (×0.5). Also capture the **full** `B046`
response (all bytes) at both SOCs — the SOC may live in a later byte than the
INT16 we currently read.
