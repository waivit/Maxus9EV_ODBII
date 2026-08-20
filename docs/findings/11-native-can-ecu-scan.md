# 11 — Native CAN: the OBD port is gateway-isolated, and a 33-ECU / 1824-DID map

- **Date:** 2026-08-20
- **Hypothesis under test:** Phase 3 Step 1 (native CAN bring-up); settles **H3**
  (regen via passive sniff); opens a large new lead set for H1/H2/H6.
- **Setup:** Waveshare ESP32-S3-RS485-CAN (`firmware/poc5_s3_can`), TWAI on
  GPIO15/16, 500 kbps, onboard 120 Ω jumper at **NC**. Harness: OBD pin 6 → CAN-H,
  pin 14 → CAN-L. Vehicle READY, parked. Read over USB serial.

## What I did
1. Desk: TWAI self-test (`NO_ACK` + self-reception) to prove driver/GPIO/transceiver.
2. Car: listen-only sniff with a 4-bitrate auto-probe (500 k / 250 k / 125 k / 1 M).
3. Diagnosed the resulting silence with a multimeter.
4. Car: active Service-22 requests, then ISO-TP multi-frame, then a full
   ECU-discovery + DID sweep of the diagnostic address range.

## Raw artifacts

### The silence, and what ruled out every alternative
```
probe 500k : no frames (bus_err=0 rx_err=0)
probe 250k : no frames (bus_err=0 rx_err=0)
probe 125k : no frames (bus_err=0 rx_err=0)
probe 1M   : no frames (bus_err=0 rx_err=0)
```
| Candidate cause | Ruled out by |
| --- | --- |
| Firmware / TWAI driver / GPIO / transceiver | desk self-test PASS (`id=0x555 DE AD BE EF` echoed) |
| Transceiver power | self-test passed on USB power alone |
| Wrong bitrate | `bus_err=0` at **all four** rates — a live bus sampled at the wrong rate raises stuff/form errors |
| Vehicle asleep | still silent with the car READY |
| Wrong terminal / termination | confirmed CAN (H/L) terminal, jumper at NC |
| **Wiring** | **multimeter: 63 Ω across CAN-H/CAN-L** (2 × 120 Ω in parallel = 60 Ω) |

### Active request — works immediately
```
tx 7E5: 03 22 B0 42 00 00 00 00
rx 7ED: 05 62 B0 42 06 19 AA AA   -> 0x0619 * 0.25 = 390.25 V
```
Reply latency ~10 ms, 5/5 requests answered. **Only `0x7ED` ever appeared** — no
other id, with an accept-all filter, on a READY vehicle.

### ISO-TP multi-frame — works
```
rx 7ED: 10 0D 62 F1 94 34 33 41   <- first frame, 13 bytes
tx 7E5: 30 00 00 ...              <- flow control (CTS, BS=0, ST=0)
rx 7ED: 21 31 33 35 34 4D 52 34   <- consecutive frame
=> F194 BMS software version = "43A1354MR4"
```
`F190` (VIN) on `7E3`/`7EB` assembled correctly over 3 frames (17 chars, valid
format). *The VIN value is deliberately not recorded here — captures are
gitignored for this reason.*

### ECU discovery — 33 responding addresses (we knew 3)
Sweep of `0x700`–`0x7FF` with `22 F1 90`, accepting **any** response id:
```
704>784 709>789 70F>77F 715>785 716>786 717>787 718>788 71C>78C 71E>78E
723>793 724>794 730>7A0 732>7A2 733>7A3 73A>7AA 73D>7AD 73E>7AE 741>7B1
743>7B3 744>7B4 747>7B7 758>7C8 75A>7CA 75B>7CB 75C>7CC 75D>7CD 775>77D
776>77E 7DF>(functional) 7E3>7EB 7E4>7EC 7E5>7ED 7F1>7F9
```
**The request→response offset is not constant:** `+0x80` (704→784), `+0x70`
(730→7A0) and `+0x08` (775→77D) all occur.

### DID sweep — 1824 readable DIDs
Ranges swept per ECU: `B0xx B3xx B4xx B7xx B9xx BAxx F1xx 00xx` (2048 each).

| ECU | supported | ECU | supported |
| --- | --- | --- | --- |
| **7E3 VCU** | **219** | 730 | 64 |
| **7E5 BMS** | **163** | 747 | 63 |
| 718 | 104 | 775 | 61 |
| 723 | 92 | 71C | 60 |
| **7E4 Gateway** | 90 | 7F1 | 47 |
| 704 | 73 | *(19 more)* | 16–43 each |

Every ECU answers `F183` (part number) and `F194` (software version), which is
how unknown modules can be identified:

| ECU | `F183` | `F194` | reading |
| --- | --- | --- | --- |
| 704 | `CB313MX100` | `BDUMFEV707` | **BDU** — battery disconnect unit |
| 709 | `CBLMIFA310` | `LSAMFEV700` | MIFA-specific |
| 716 | `MIFAFBLV04` | `EPX1-SW-06` | |
| 718 | `      0006` | `0774100ADU` | also stores the VIN at `B019` |
| 730 | `SCM0323V05` | `SWV3.81107` | |
| 732 / 733 | `B00A29MIFA` | `A00A52MIFA` | identical pair (two of the same module) |

### Per-cell module data — and it is single-frame
Note 09 predicted per-cell stats at `B001, B009, B011, B019, B021, B029, B0A3…`
and expected them to need multi-frame. They return **7-byte single frames**:
```
[7E5] B001 + 7 : 14 50 14 5E 93 92 A6
[7E5] B009 + 7 : 14 54 14 65 94 94 AA
[7E5] B011 + 7 : 14 58 14 68 93 93 AE
```
Twelve groups exist (stride 8): `B001 B009 B011 B019 B021 B029` and
`B0A3 B0AB B0B3 B0BB B0C3 B0CB`. Applying the MG formulas:
`0x1450 = 5200 → /2000 + 1.0 = 3.600 V`; `0x93 = 147 → ×0.5 − 40 = 33.5 °C`.
Both are consistent with the pack state at the time (cell max 3.618 V, ~33 °C),
and all twelve groups read within 3.600–3.612 V — a well-balanced pack.

Also confirmed in the same sweep: `B058 + 3 : 0E 1C 1A` — the third byte (`26`)
is the **cell index**, and `B045 = 3F FF` is the sentinel recorded in note 10.

### Negative-response codes worth acting on
| NRC | meaning | seen on | so what |
| --- | --- | --- | --- |
| `0x31` | requestOutOfRange | ~2000/ECU | normal "no such DID" |
| **`0x78`** | **responsePending** | `7F1` (many), `7E3 B9E4/F130` | **the ECU intended to answer** — our scanner treated it as final and moved on, so those DIDs are still unread |
| `0x22` | conditionsNotCorrect | `7E3 B9E5`, `F131` | DID exists but the vehicle state blocks it — retry while driving/charging |
| `0x33` | securityAccessDenied | `7E3 F1A7` | locked behind security access |

## Observations
- Passive listening on the OBD port returns **nothing at all** while a request
  gets an answer in ~10 ms over the same wires. With 63 Ω measured and the
  transceiver self-test passing, the only consistent explanation is that the port
  is behind a **gateway that answers requests but does not forward the vehicle's
  internal bus traffic**.
- The native transport is far faster than the ELM327 text layer (~10 ms/PID vs
  hundreds), which is what makes a 2048-DID × 33-ECU sweep practical at all.
- The non-constant request→response offset means an ECU scan **must** accept any
  response id. Assuming the OBD-standard `+8` would have found 3 ECUs, not 33.

## Interpretation
The OBD connector on this vehicle is a **diagnostic-only endpoint**, not a tap
onto a live bus. That is a property of the vehicle, not a fault in our hardware —
and it is decisive for what Phase 3 can and cannot deliver through this port.

The flip side is that request/response works extremely well, and the address
space is far richer than the ELM327 path ever suggested: the BMS alone exposes
163 readable DIDs where we had been using about ten.

## Verdict
- **Phase 3 Step 1: PASS**, but on a **revised criterion**. "Frames arrive
  passively" is unachievable here by design; the transport is instead proven by a
  correct request/response round trip. The roadmap is updated to match.
- **Steps 2 and 3 also pass** in the same session: active Service 22 decodes a
  confirmed PID, and ISO-TP multi-frame delivers `F194` and `F190`.
- **Step 4 (passive sniffing) is not possible via the OBD port** — proven, not
  assumed.
- **H3 (regen level) → rejected via OBD.** It was parked pending a passive sniff;
  that route is now closed at this connector. Reaching it requires tapping an
  internal bus elsewhere in the vehicle, which is a different scale of work.
- **New leads:** 12 per-cell module groups (single-frame), 1824 readable DIDs
  across 33 ECUs, `F183`/`F194` as an ECU-identification method.
- **Known gap in our own tooling:** NRC `0x78` (responsePending) is not handled —
  those DIDs are unread, not unsupported.

## Next
1. **Handle `0x78`**: after a responsePending, keep reading until the real reply
   arrives. Re-run the affected ECUs (`7F1` especially) to recover missed DIDs.
2. **Re-scan under different vehicle states** (driving, charging) and diff, to
   catch the `0x22` conditionsNotCorrect DIDs and to identify unknown ECUs by
   which values move — the `scanner.py` snapshot/diff idea applied at scale.
3. **Decode the per-cell groups** properly (12 groups × 2 voltages + 2 temps) and
   surface them on the dashboard.
4. **SOC**: the dash only displays SOC % while charging, so the settled low-SOC
   calibration point (H1) must be captured during a charge session. `B046` read
   `0x00FA/0x00FB` = 25.0–25.1 % Raw during this session.
