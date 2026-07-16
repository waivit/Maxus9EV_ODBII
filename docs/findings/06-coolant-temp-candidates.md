# 06 — Coolant / radiator temperature candidates (offline)

- **Date:** 2026-07-16
- **Hypothesis under test:** H6 — a coolant / radiator temperature is exposed on a
  BMS (7E5) PID.
- **Setup:** offline analysis (no car) of the scan capture
  `captures/20260716_104946_7E5_B0_scan061-0FF.jsonl` (7E5, B061–B0FF, 104 positive
  PIDs), taken 2026-07-16 with the vehicle READY at idle.

## What I did
Applied the confirmed temperature decode **`byte/2 − 40`** (from Batt Temp High/Low
B056/B057, which read 30 °C at byte `0x8C`) to every byte of every positive PID in
the capture, and shortlisted values in a plausible thermal range.

## Raw artifacts (decisive bytes preserved; full .jsonl is git-ignored)

| PID | raw data (hex) | temp decode | note |
| --- | --- | --- | --- |
| **B0E3** | `8E` (142) | **31 °C** | single byte, exactly battery temp; was the secondary SOC look-alike |
| **B0A3** | `1547 1554 8C8C A0` | 30 / 30 / **40 °C** | repeating per-module record |
| B0AB | `154C 1554 8C8C A9` | 30 / 30 / 44.5 °C | " |
| B0B3 | `154F 1554 8C8C A8` | 30 / 30 / 44 °C | " |
| B0BB…B0DE | `…8C8C 9F…A5` | 30 / 30 / 39.5–42.5 °C | " (byte[6] varies per record) |
| **B0D6** | `0090 008E 0E99…` | 32 / 31 °C | summary block, temps mixed with other fields |
| B0A0 | `00 94 C1 20` | byte1 → 34 °C | weaker |

## Observations
- The two constant `8C 8C` bytes in the module records = the known 30 °C battery
  temps. The trailing `byte[6]` in those records varies 39–44 °C — **warmer than the
  pack** — a per-module hotspot / component-temp candidate.
- `B0E3 = 142` decodes to 31 °C, matching the battery temp exactly. This is the same
  raw value flagged as a secondary SOC look-alike in `05-soc-candidate-b05c.md`; a
  temperature reading fits it better than SOC.

## Interpretation
Several plausible temperature fields exist on 7E5, but **an idle snapshot cannot
isolate a coolant temperature** — with the car at rest everything thermal sits near
ambient ~30 °C, so temp candidates are indistinguishable from battery temps or
coincidental byte values. The discriminator must be dynamic: the coolant/loop temp
is the value that **rises under thermal load**.

**System-level note:** this capture is **BMS-only (7E5)**, so any temperature here
belongs to the **battery cooling loop**. An EV "radiator" (หม้อน้ำ) usually refers to
the **motor / inverter cooling loop**, which most likely lives on the **VCU (7E3)** —
a header not covered by this capture.

## Verdict
- **H6 → testing.** Candidates shortlisted (B0E3, the module-record byte[6], B0D6),
  but none confirmed — an idle snapshot is insufficient to identify coolant temp.
- Side effect for **H1:** `B0E3` is more likely a temperature than SOC, which
  slightly strengthens `B05C` as the sole SOC lead.

## Next (needs the car)
1. **Live delta test (7E5):** watch `B0E3` and the module-record `byte[6]` while the
   battery/thermal system heats up (sustained drive or DC fast-charge). The true
   coolant/loop temp tracks the heating.
2. **Scan VCU 7E3** (capture-enabled) for the motor/inverter (radiator) loop temp —
   not present in the BMS capture.
