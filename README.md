# MAXUS 9 EV - OBD2 Data Reader

Read battery and drivetrain data from a MAXUS 9 EV — over an ELM327 Bluetooth
adapter, or natively over CAN with an ESP32-S3.

## Project goals

1. **Read the regenerative braking level (High / Medium / Low).** ⭐ *Primary.*
   Tracked as **H3** in [`docs/findings/00-hypotheses.md`](docs/findings/00-hypotheses.md).
   A passive CAN sniff cannot reach it — the OBD port is gateway-isolated
   ([`11-native-can-ecu-scan.md`](docs/findings/11-native-can-ecu-scan.md)) — so the
   method is to scan every DID at each regen level and diff the captures with
   `tools/analyze_scan.py diff`.
2. **Read accurate battery state** — SOC, SOH, temperatures, per-cell voltages.
3. **A standalone in-car device** that serves a live dashboard to a phone, with no
   PC and no phone app (Phase 2 / Phase 3 roadmaps in [`docs/`](docs/)).

Everything is **read-only**: only UDS Service 22 reads and ISO-TP flow control are
ever transmitted. Nothing is written to the vehicle.

## Hardware

- ELM327 Bluetooth (NEXPEAK NX101 PRO recommended, PIC18F25K80 chip)
- Windows PC with Bluetooth
- MAXUS 9 EV in READY state
- *(Phase 3)* Waveshare ESP32-S3-RS485-CAN for native CAN — TWAI on GPIO15/16,
  120 Ohm termination jumper at **NC** when tapping the vehicle

## Setup

```bash
pip install -r requirements.txt
```

## Tools

| Tool | Command | Purpose |
|---|---|---|
| reader | `cd src && python reader.py` | Read confirmed PIDs + live loop |
| scanner | `cd src && python scanner.py` | Scan unknown PID ranges, diff snapshots |
| diag | `cd src && python diag.py` | Battery health dashboard + OK/WARN/ALARM alerts |
| webdash | `cd src && python webdash.py` | Web dashboard (LAN) — raw hex + ASCII + decoded, auto-refresh |
| split scan | `python tools/split_scan_log.py <capture>` | Split a native-CAN scan capture into one log per ECU |
| analyze scan | `python tools/analyze_scan.py ids\|decode\|diff ...` | Identify ECUs, show candidate decodes, **diff two captures** (the regen hunt) |

Or use **VSCode Run & Debug** (F5) and select a launch config.

## Discovery loop & findings

This is reverse-engineering work, so it follows a lab-notebook loop:
**research → hypothesize → test → verify**, and log everything.

- Open questions live in [`docs/findings/00-hypotheses.md`](docs/findings/00-hypotheses.md).
- Each investigation round is a numbered note built from
  [`docs/findings/TEMPLATE.md`](docs/findings/TEMPLATE.md).
- What is already proven — and *how* — is in
  [`docs/findings/01-confirmed-pids.md`](docs/findings/01-confirmed-pids.md).

The tables below are the quick overview; the finding notes are the deeper record.

---

## Confirmed PIDs

| Parameter | Header | PID | Formula | Unit |
|---|---|---|---|---|
| Bus Voltage | 7E5 | 22 B0 41 | INT16 * 0.25 | V |
| Pack Voltage | 7E5 | 22 B0 42 | INT16 * 0.25 | V |
| Pack Current | 7E5 | 22 B0 43 | INT16 * 0.025 - 1000 | A |
| Batt Temp High | 7E5 | 22 B0 56 | byte/2 - 40 | °C |
| Batt Temp Low | 7E5 | 22 B0 57 | byte/2 - 40 | °C |
| Cell V Max | 7E5 | 22 B0 58 | INT16 * 0.001 | V |
| Cell V Min | 7E5 | 22 B0 59 | INT16 * 0.001 | V |
| 12V Battery | 7E3 | 22 01 12 | byte * 1.0 | V |
| Charge Status | 7E5 | 22 B0 48 | byte | - |

## ECU Map

```
Request 7E3 -> Response 7EB  (IMCU - Integrated Motor Control Unit)
Request 7E4 -> Response 7EC  (CCU  - Central Control Unit / gateway)
Request 7E5 -> Response 7ED  (BMS  - Battery Management System)
```

A native-CAN scan found **33 responding ECUs and 1824 readable DIDs**, and the
request->response offset is *not* always the OBD-standard +8. See
[`11-native-can-ecu-scan.md`](docs/findings/11-native-can-ecu-scan.md) and
[`12-ecu-identification-and-bms-structure.md`](docs/findings/12-ecu-identification-and-bms-structure.md).

## Still Unknown

See [`docs/findings/00-hypotheses.md`](docs/findings/00-hypotheses.md) for status and next tests.

| Parameter | Hyp | Notes |
|---|---|---|
| SOC | H1 | B046/10 = Raw SOC; dash = Raw + ~4%. Needs one settled low-SOC point to lock. |
| SOH | H2 | B061/100 = 94.3%, consistent across sessions. Needs a dealer/dash SOH to confirm. |
| Charge Status | H4 | B048 enum not fully decoded (3 = driving?). |
| **Regen Level** | **H3** | ⭐ **Primary goal.** Passive sniff ruled out; hunt it by diffing DID scans at Low/Medium/High. |

## Protocol

- CAN 11-bit 500kbps (ATSP6)
- Service 22 (UDS Read by Identifier)
- Positive response: 62 + DID + data
- Negative response: 7F + service + NRC

## Next Steps

1. Scan SOC in range B041-B07F on header 7E5
2. Try ESP32 + TJA1050 passive sniff for regen level CAN frame
3. Correct SOH formula (raw byte analysis needed)
