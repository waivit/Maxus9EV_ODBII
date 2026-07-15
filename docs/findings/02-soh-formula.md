# 02 — SOH formula for B0 4C (wrong scaling)

- **Date:** 2026-07-15 (opened — investigation not yet run at the vehicle)
- **Hypothesis under test:** H2 — SOH is `B0 4C` on 7E5 but the formula is wrong.
- **Setup (planned):** ELM327 on 7E5, vehicle READY. Need a **reference SOH** for
  calibration (dealer/service tool, or the dash/app battery-health readout).

## What I did
Nothing at the car yet. This note captures the current state and a turnkey method
so the next hardware session goes straight to data.

## Raw artifacts
- Current code (`src/pids.py` `UNDER_INVESTIGATION`):
  `formula = INT16 × 0.1 if len(b) > 1 else b[0] × 0.5` → **reads ≈ 1.5 %**.
- **TODO at the car:** paste the raw frame here, e.g.
  `7E5 22 B0 4C` → `7ED __ 62 B0 4C <bytes...>`.
- **TODO:** reference SOH value from an independent source (e.g. dash = 9_ %).

## Observations
- Output ≈ 1.5 % is implausible for a working pack — the scaling or the byte
  layout (single byte vs INT16) is wrong, or `B04C` is not SOH at all.
- With `INT16 × 0.1 = 1.5`, the raw INT16 would be 15 (bytes `00 0F`); with
  `byte × 0.5 = 1.5`, the raw byte would be 3. Both are suspiciously small for a
  health percentage — a hint the real value lives in a different byte position or
  needs a different scale/offset.

## Interpretation (candidate scalings to test)
Once the raw bytes are captured, try these against the reference SOH:

| Candidate | Formula | Use when |
| --- | --- | --- |
| A | `byte × 1.0` | single byte already = percent (e.g. `0x5F` = 95) |
| B | `byte × 0.5` | 0–200 raw maps to 0–100 % |
| C | `INT16 × 0.1` | 0–1000 raw maps to 0–100 % (current guess) |
| D | `INT16 × 0.01` | 0–10000 raw maps to 0–100 % |
| E | offset variants, e.g. `byte − 100`, or 2nd byte instead of 1st | if raw sits in b[1] |

Pick the candidate whose output matches the reference within ~1–2 %.

## Verdict
- **H2 → inconclusive (testing).** Formula confirmed wrong; correct one unknown
  until raw bytes + a reference SOH are captured.

## Next (single most informative test)
At the car: read `22 B0 4C` on 7E5, record the full raw response **and** an
independent SOH reference in the same session. Then evaluate candidates A–E above,
update `src/pids.py`, and set H2 → confirmed here + in `00-hypotheses.md`.
