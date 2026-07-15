# MAXUS 9 EV - OBD2 Data Reader

Read battery data from MAXUS 9 EV via ELM327 Bluetooth adapter.

## Hardware

- ELM327 Bluetooth (NEXPEAK NX101 PRO recommended, PIC18F25K80 chip)
- Windows PC with Bluetooth
- MAXUS 9 EV in READY state

## Setup

```bash
pip install -r requirements.txt
```

## Tools

| Tool | Command | Purpose |
|---|---|---|
| reader | `cd src && python reader.py` | Read confirmed PIDs + live loop |
| scanner | `cd src && python scanner.py` | Scan unknown PID ranges, diff snapshots |
| diag | `cd src && python diag.py` | Battery health dashboard + alerts *(planned)* |

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
Request 7E3 -> Response 7EB  (VCU - Vehicle Control Unit)
Request 7E4 -> Response 7EC  (Gateway ECU)
Request 7E5 -> Response 7ED  (BMS - Battery Management System)
```

## Still Unknown

See [`docs/findings/00-hypotheses.md`](docs/findings/00-hypotheses.md) for status and next tests.

| Parameter | Hyp | Notes |
|---|---|---|
| SOC | H1 | PID B040 = NRC 0x31. Range B04x not fully scanned. |
| SOH | H2 | Returns 1.5% - formula wrong ([`02-soh-formula.md`](docs/findings/02-soh-formula.md)). |
| Charge Status | H4 | B048 enum not fully decoded (3 = driving?). |
| Regen Level | H3 | Not exposed via OBD2. On infotainment CAN bus (parked). |

## Protocol

- CAN 11-bit 500kbps (ATSP6)
- Service 22 (UDS Read by Identifier)
- Positive response: 62 + DID + data
- Negative response: 7F + service + NRC

## Next Steps

1. Scan SOC in range B041-B07F on header 7E5
2. Try ESP32 + TJA1050 passive sniff for regen level CAN frame
3. Correct SOH formula (raw byte analysis needed)
