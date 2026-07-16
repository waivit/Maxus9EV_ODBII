# 05 — SOC candidate B0 5C (byte × ~0.4)

- **Date:** 2026-07-16
- **Hypothesis under test:** H1 — SOC lives on a `B04x`/`B05x` PID on header 7E5.
- **Setup:** ELM327 v1.5 on COM7 (Bluetooth), vehicle READY (not charging),
  ~40 min into the session. Dash SOC ≈ **50–55 %** (user-reported, approximate).
  Scan run through the capture-enabled `scanner.py`.

## What I did
- Ran a full scan of **B061–B0FF on 7E5** (159 PIDs, 104 positive) with raw output
  saved to `captures/20260716_104946_7E5_B0_scan061-0FF.jsonl` (git-ignored — may
  contain vehicle-specific data).
- Re-read the leading candidate `B05C` three times to check stability.

## Raw artifacts
The full capture is on disk (git-ignored); the decisive bytes are preserved here.

- `7E5 22 B0 5C` → `7ED 04 62 B05C 8D` → **`B05C = 0x8D = 141`**, three identical
  reads (`7ED0462B05C8D` ×3). Prior session (~40 min earlier) read `0x8F = 143`.
- Supplementary look-alike: `B0E3 = 0x8E = 142` (single byte).
- Reference: dash SOC ≈ 50–55 %.

## Observations
- `B05C` is stable within a session and **decreased 143 → 141** over ~40 min of
  READY idle — consistent with slow discharge (see H5).
- Scalings against the dash reading:

  | Formula | B05C=141 → | Fits 50–55 %? |
  | --- | --- | --- |
  | `byte ÷ 2` (old guess) | 70.5 % | ✗ ruled out |
  | `byte ÷ 255 × 100` | 55.3 % | ✓ |
  | `byte × 0.4` (0–250 scale) | 56.4 % | ✓ |

## Interpretation
`B05C` is a strong SOC candidate with a scale in the **`byte × ~0.39–0.40`**
family; `÷ 2` is rejected. **Caveat:** 141 sits mid-scale, so several near-0.4
scalings all land ≈ 55 % — a single approximate mid-range point can reject ÷2 but
**cannot pin the exact scale**. `B0E3` (=142) is a secondary look-alike; could be a
duplicate/derived value or coincidence. Confidence: moderate.

## Verdict
- **H1 → testing (elevated, NOT confirmed).** Evidence supports B05C as SOC and
  narrows the formula, but one approximate, mid-scale data point is not enough to
  confirm the PID or fix the scaling. User agreed to keep it open.

## Next
After driving the car for a while and recharging, read `B05C` (and `B0E3`) at a
**clearly different SOC** (near full or near empty) and fit the scale across the
two points. Only then confirm H1 and update `pids.py`.
