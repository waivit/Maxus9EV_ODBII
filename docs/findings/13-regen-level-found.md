# 13 — Regen level found: `7E3 B480` (the primary project goal)

- **Date:** 2026-08-21
- **Hypothesis under test:** **H3** — regen level (High / Medium / Low) is readable
  via Service 22 from whichever ECU owns the setting.
- **Setup:** `firmware/poc5_s3_can` in MODE_DASH with the scan-over-WiFi and OTA
  endpoints. Board in the car on the OBD port, vehicle READY and parked, joined
  to the house WiFi. Every scan triggered and downloaded over the network — no
  laptop in the car, no USB.

## What I did
Ran the same DID sweep three times with the vehicle otherwise unchanged, altering
only the regen selector: **Low**, then **Medium**, then **High**. Scanned `7E3`
(IMCU) and `7E4` (CCU) — the two ECUs ranked most likely in note 12 — then diffed
the three captures, requiring a candidate to differ across **all three**.

## Raw artifacts

Three captures, 309 DIDs present in all of them:

| step | result |
| --- | --- |
| DIDs read in all three scans | 309 |
| changed in at least one step | 31 |
| **distinct in all three** | **5** |
| **stepping monotonically in order** | **1** |

The five that differed everywhere, and what happened to four of them:

| DID | Low | Medium | High | verdict |
| --- | --- | --- | --- | --- |
| **`7E3 B480`** | **`00`** | **`01`** | **`02`** | ✅ **monotonic, in order** |
| `7E3 B065` | 383316 | 400091 | 411335 | a counter — rises with time regardless |
| `7E4 B022` | 28 | 30 | **29** | not monotonic |
| `7E4 B023` | 400 | 427 | **418** | not monotonic |
| `7E4 B025` | 63 | 68 | **66** | not monotonic |

Live verification, after adding `B480` to the dashboard and pushing it by OTA:
```
read B480 = 01  ->  predicted "Medium" before being told
```
The prediction was made without knowing the current setting (it had been changed
after the last scan) and the user confirmed the dashboard matched the car.

## Observations
- Of 309 DIDs, exactly one moves through three distinct values **in selector
  order**. The other four candidates each break the ordering or are explained by
  time.
- `B480` sits on **`7E3` (IMCU, the motor control unit)** — the module that
  actually applies regenerative torque — and in the **`B4xx` page**, alongside the
  already-confirmed `B401` torque, `B402` motor speed and `B405` motor temp.
- A two-state test would **not** have been enough: five candidates survive a
  Low→High diff and only the third level separates them.

## Interpretation
`B480` is the regen level. The value encoding is a plain 3-position enum
(`00` Low, `01` Medium, `02` High), the address is mechanistically the right one,
and a blind prediction from the value matched the car.

The earlier verdict in note 11 — that regen would need a tap on an internal bus —
was wrong because it assumed passive sniffing was the only route. Sniffing is
indeed impossible at this connector, but the setting is stored, and stored values
are what Service 22 reads. Making regen a stated primary goal is what forced that
assumption back into the open.

## Verdict
- **H3: CONFIRMED.** `22 B4 80` on header `7E3` (response `7EB`);
  `00` = Low, `01` = Medium, `02` = High.
- **The project's primary goal is met** — regen level is readable.
- The three-level requirement is vindicated as a method, not just caution.

## Next
1. **Can it be written?** Untested. `Service 2E` exists, but this ECU already
   returned NRC `0x33` (securityAccessDenied) on `F1A7`, so a write may well be
   locked. The safe probe is to **write back the value it already holds**: a
   rejection tells us it is locked, and an acceptance changes nothing.
2. If writes work, the intended use is a **physical paddle on a board GPIO**
   rather than an on-screen control — reaching for a touchscreen while driving is
   the more dangerous option, and a paddle also matches how engine braking is
   normally operated.
3. Automatic regen (by following distance, brake input, driver profile) needs two
   inputs we do not have yet: a **brake-pedal DID** and a **distance DID**. Both
   are findable with the same scan-and-diff method — brake is easy to control for,
   distance needs an object placed at varying ranges.
4. Any automatic control belongs behind a **shadow mode** first: compute the
   decision and log it while driving, change nothing, and review the log.
