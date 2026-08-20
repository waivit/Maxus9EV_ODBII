# 10 — poc4 first live car test: confirmed-9 cross-check + new leads + SOC resolved

- **Date:** 2026-08-14
- **Hypothesis under test:** validates the `uds.c` port (Step-4 gate); advances
  H1 (SOC), H2 (SOH), H4 (B048), H6 (coolant); promotes several VCU leads.
- **Setup:** ESP32 (WROOM-32) `poc4_idf_decode` flashed, BT→ELM327 (OBDII,
  legacy PIN), 29-PID table + tabbed dashboard served over the ESP32's own WiFi
  AP (`192.168.4.1`). Vehicle READY, then driven (idle vs moving compared).
  Data captured as phone screenshots per tab (`docs/*.jpg`).

## What I did
Read the dashboard on the phone at two states — **stationary** (poll #2/#3) and
**driving** (poll #16/#19) — across all three tabs (BMS / Motor / Vehicle),
recording each PID's raw hex and decoded value. Separately compared the bridge's
SOC against the car's own dash SOC at two settled points.

## Raw artifacts

### Confirmed-9 — every value decodes correctly on the ESP32 (poll #16, driving)
| PID | header | raw | formula | ESP32 value |
| --- | --- | --- | --- | --- |
| Bus Voltage | 7E5 | `0664` | INT16×0.25 | 409.0 V |
| Pack Voltage | 7E5 | `0664` | INT16×0.25 | 409.0 V |
| Pack Current | 7E5 | `A121` | (INT16−40000)/40 | **31.2 A (+ = discharging while driving — H5 ✓)** |
| Cell V Max | 7E5 | `0ECF` | INT16×0.001 | 3.791 V |
| Cell V Min | 7E5 | `0EC1` | INT16×0.001 | 3.777 V |
| Batt Temp Hi | 7E5 | `91` | b[0]×0.5−40 | 32.5 °C |
| Batt Temp Lo | 7E5 | `8F` | b[0]×0.5−40 | 31.5 °C |
| 12V Battery | 7E3 | `0D` | b[0]×1 | **13.0 V (MAXUS uses ×1, not MG's /10 ✓)** |
| Charge Status | 7E5 | `03` | enum | Running (03) |

### New VCU leads — proven live by idle→driving delta (7E3)
| PID | metric | idle (poll #2) | driving (poll #19) | raw (driving) | formula |
| --- | --- | --- | --- | --- | --- |
| `B405` | Motor Temp | 34 °C | 36 °C | `4C` | b[0]−40 |
| `B402` | Motor Speed | 0 rpm | **1106 rpm** | `8451` | INT16−0x7FFF |
| `B401` | Motor Torque | 0 | **330** | `8149` | INT16−0x7FFF |
| `E101` | Odometer | — | 61911 km | `00F1D7` | 3-byte km |

### Other testing PIDs read
| PID | metric | value | raw | note |
| --- | --- | --- | --- | --- |
| `B061` | SOH | 94.3 % | `24D6` | INT16/100; consistent with note 07 (~94.9 %) |
| `B05C` | Coolant | 32.5 °C | `91` | **identical to Batt Temp Hi** — pending thermal-delta test |

## Observations

**Confirmed-9 port is correct.** Every raw→value matches the proven Python
formulas exactly. Pack Current is **positive while driving** = discharging,
confirming the H5 sign convention on-device. 12V reads 13.0 V with `×1` (MG's
`/10` would give 1.3 V) — MAXUS-specific formula validated again.

**Motor PIDs work and track physical state.** Motor Speed 0→1106 rpm and Torque
0→330 between idle and driving; Motor Temp rises 34→36 °C. These VCU (7E3) DIDs
are now proven on MAXUS, not just MG leads.

**DIDs that MAXUS does not answer (dashboard shows `--`):**
`B309` (VCU coolant), `005B` (SOC-alt), `0046` (ambient), `B900` (gear),
`B18C` (ignition), `B703` (HV contactor), `BA40` (handbrake), `BA00` (speed),
`B712` (charge rate), `000D` (speed).
Critically, **other 7E3 DIDs answered fine in the same cycle** (Motor Temp/Speed/
Torque, Odometer, 12V), so this is **not a header problem** — these specific DIDs
simply do not exist on MAXUS (they are MG ZS EV DIDs we inherited as leads).

**Sentinel/garbage values (must be filtered, not real):**
- `B045` "Resistance" → 8191.5 Ohm, raw `3FFF` (absurd for a pack; sentinel).
- `B0CE` "BMS Range" → 65534, raw `FFFE` (0xFFFE unavailable sentinel).

## The SOC resolution (H1)

Two **settled** paired readings (dash vs bridge `B046÷10`):

| dash SOC | bridge (B046÷10) | B046 INT16 | dash − bridge |
| --- | --- | --- | --- |
| 49 % | 45.2 | 452 | +3.8 |
| 80 % | 75.2 | 752 | +4.8 |

- The old suspect point (dash 80 → **57.2**, gap +22.8, `08-soc-b046-live.md`) is
  now **superseded** by dash 80 → **75.2** (gap +4.8). The **staleness theory is
  confirmed**: the 57.2 reading was Raw SOC that had not yet updated right after
  charging.
- `B046 ÷ 10` = **Raw SOC**, and the dash ≈ **Raw + ~4 %** (offset 3.8→4.8, near
  constant). A 2-point linear fit gives **`Display ≈ 1.033 × Raw + 2.3`** — gain
  **~1.03**, *not* MG's 1.1, confirming the transform is BMS-version-specific
  (MAXUS ≠ MG ZS).
- With only two points, a constant offset (`Raw + 4`) and the linear fit are
  indistinguishable (they diverge < 1 % across the whole range). Either is already
  accurate to ~±1 %.

## Interpretation
The Step-4 port is validated: all confirmed PIDs decode correctly on the ESP32,
and the idle/driving delta independently proves four VCU motor/odometer PIDs. The
SOC mystery collapses to a **~+4 % offset** — `B046` was always the right PID; the
earlier large error was a single stale post-charge sample. The MG leads split
cleanly into *works on MAXUS* (motor group, SOH, coolant) vs *does not exist on
MAXUS* (the `--` list) vs *sentinel* (`B045`, `B0CE`).

## Verdict
- **Step-4 cross-check: PASS.** `uds.c` + the PID table are correct on-device.
- **H1 (SOC):** `B046÷10` = Raw SOC; dash = Raw + ~4 % (gain ~1.03). Staleness
  theory confirmed. Still `testing` only because a **low-SOC third point** is
  needed to lock offset-vs-linear; the formula is otherwise ~±1 % usable.
- **H2 (SOH):** `B061/100` = 94.3 %, consistent across two sessions; still needs a
  dealer/dash SOH to confirm absolute accuracy.
- **H4 (B048):** `03` = Running seen again while driving (READY) ✓.
- **H6 (coolant `B05C`):** reads, but equals Batt Temp Hi exactly → still needs a
  thermal-delta test to prove it is a distinct coolant sensor.
- **New confirmed (MAXUS):** `B405` motor temp, `B402` motor speed, `B401` torque,
  `E101` odometer.
- **Rejected on MAXUS (DID absent):** `B309`, `005B`, `0046`, `B900`, `B18C`,
  `B703`, `BA40`, `BA00`, `B712`, `000D`.
- **Filter as sentinel:** `B045` (raw `3FFF`), `B0CE` (raw `FFFE`).

## Next
1. **Lock the SOC transform:** capture one settled paired point at **low SOC
   (~20–30 %)** to decide `Raw + 4` vs `1.033·Raw + 2.3`, then apply to the
   dashboard (drop the `test` tag on SOC).
2. **Coolant delta:** read `B05C` when the pack is warm (after a fast charge or a
   hard drive) to see if it separates from Batt Temp Hi.
3. **Prune the table:** consider removing the 10 dead DIDs and the 2 sentinels from
   the poll to speed up the cycle (or move them to a hidden "raw" tab).
4. **SOH confirmation:** obtain a dealer/dash SOH % to validate `B061/100`.
