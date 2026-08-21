# 01 — Confirmed PIDs (BMS + IMCU) and the ECU map

- **Date:** 2026-07-15 (migrated from earlier reverse-engineering sessions)
- **Hypothesis under test:** — (baseline record of what is already proven)
- **Setup:** ELM327 Bluetooth (NEXPEAK NX101 PRO, PIC18F25K80), Windows + pyserial;
  CAN 11-bit / 500 kbps (`ATSP6`), headers on (`ATH1`), Service 22 (UDS Read by
  Identifier). Vehicle in READY state.

## What I did
Queried Service 22 PIDs across the SAIC/MAXUS ECUs and recorded which return a
positive response (`62 + DID + data`) versus a negative response (`7F + svc + NRC`).
Decoded each positive payload into a physical value.

## Raw artifacts

**ECU request → response map**

```
Request 7E3 -> Response 7EB   (IMCU - Integrated Motor Control Unit)
Request 7E4 -> Response 7EC   (CCU  - Central Control Unit / gateway)
Request 7E5 -> Response 7ED   (BMS  - Battery Management System)
```

> **Naming corrected 2026-08-21** (`12-ecu-identification-and-bms-structure.md`):
> `7E3` was called "VCU" here and in notes 03/06/07/09 — a name inherited from the
> MG cross-reference, not from this vehicle. Read via the standard ISO 14229
> identification DIDs, `7E3` reports `F183 = "IMCU…"` / `F194 = "MIFAMCU520"`, and
> every confirmed PID on it is motor-related. `7E4` likewise reports `CCU_S001R3`.
> **Addresses, response ids and all formulas are unchanged — only the names were
> wrong.** The dated notes keep their original wording as a record.

There are **33 responding ECUs in total** (`0x700`–`0x7FF`), not three — see
`11-native-can-ecu-scan.md`. The three above are simply the ones reachable and
useful over the ELM327 path.

## Observations — confirmed working PIDs

Mirror of `src/pids.py` `CONFIRMED`. `INT16` = `(b[0]*256)+b[1]`.

| Parameter | Header | Command | Formula | Unit | Notes |
| --- | --- | --- | --- | --- | --- |
| Bus Voltage | 7E5 | `22 B0 41` | INT16 × 0.25 | V | HV battery bus voltage |
| Pack Voltage | 7E5 | `22 B0 42` | INT16 × 0.25 | V | HV battery pack voltage |
| Pack Current | 7E5 | `22 B0 43` | INT16 × 0.025 − 1000 | A | + = charging, − = discharging |
| Batt Temp High | 7E5 | `22 B0 56` | b[0] / 2 − 40 | °C | |
| Batt Temp Low | 7E5 | `22 B0 57` | b[0] / 2 − 40 | °C | |
| Cell Voltage Max | 7E5 | `22 B0 58` | INT16 × 0.001 | V | |
| Cell Voltage Min | 7E5 | `22 B0 59` | INT16 × 0.001 | V | |
| 12V Battery | 7E3 | `22 01 12` | b[0] × 1.0 | V | on IMCU (7E3), not BMS |
| Charge Status | 7E5 | `22 B0 48` | b[0] | — | enum, values TBD (3 = driving?) → H4 |

## Observations — known dead ends (NRC 0x31 requestOutOfRange)

Mirror of `src/pids.py` `NOT_AVAILABLE`.

| Header | Command | Result |
| --- | --- | --- |
| 7E5 | `22 B0 40` | NRC 0x31 — the "obvious" SOC PID is out of range |
| 7EC | `22 D0 08` | NRC 0x31 — SOC via gateway also rejected |

## Interpretation
The BMS (7E5) is the richest source: voltage, current, cell min/max, and pack
temperatures all decode cleanly with simple linear formulas. Current is signed via
a −1000 offset so charge/discharge is distinguishable. SOC and SOH are **not** on
their intuitive PIDs (`B040` / gateway `D008` both NRC 0x31), which is what pushes
those into open hypotheses.

## Verdict
- **Baseline confirmed.** 9 PIDs proven and in daily use by `reader.py`. These are
  the trustworthy inputs for the diag tool and for calibrating unknown PIDs.

## Next
- SOC: scan `B041`–`B07F` on 7E5 for an untried positive PID (H1).
- SOH: fix the `B04C` formula (H2 → `02-soh-formula.md`).
- Charge Status: enumerate `B048` values across states (H4).
