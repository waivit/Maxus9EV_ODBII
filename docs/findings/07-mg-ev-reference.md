# 07 — MG EV (OVMS) as a SAIC-platform reference

- **Date:** 2026-07-16
- **Hypothesis under test:** cross-references H1 (SOC), H2 (SOH), H4 (BMS status),
  H6 (coolant temp).
- **Setup:** offline — reading the OVMS `vehicle_mgev` firmware (open source). MG EV
  is a **SAIC** vehicle, same manufacturer/platform family as MAXUS 9. Source:
  https://github.com/MG-EV-Hackers/Open-Vehicle-Monitoring-System-3-MGEV
  (`components/vehicle_mgev/src/mg_obd_pids.h`, `mg_poll_bms.cpp`).

## Why this reference is trustworthy for us
The 6 PIDs we independently confirmed decode with the **identical** formula in the
MG firmware — proof the SAIC BMS decode transfers:

| PID | MG formula | Our confirmed formula | |
| --- | --- | --- | --- |
| B041 bus V | value/4 | INT16×0.25 | same |
| B042 pack V | value/4 | INT16×0.25 | same |
| B043 current | (value−40000)/40 | INT16×0.025−1000 | same |
| B056 temp | data[0]×0.5−40 | byte/2−40 | same |
| B058/B059 cell V | value/1000 | INT16×0.001 | same |

## What the reference resolves (matched against OUR captured raw)

`INT16 = data[0]×256 + data[1]`.

| Topic | MG PID | MG formula | Our raw | Our decode | Cross-check |
| --- | --- | --- | --- | --- | --- |
| **SOC (H1)** | **B046** | `(raw−lo)×100/(hi−lo)` | `02 2D` = 557 | ≈55.7% (lo=0,hi=1000) | dash 50–55% ✓ |
| **SOH (H2)** | **B061** | INT16/100 | `25 12` = 9490 | 94.9% | plausible, no ref yet |
| **Coolant (H6)** | **B05C** | data[0]×0.5−40 | `8D` = 141 | 30.5°C | = idle ambient |
| **BMS status (H4)** | **B048** | enum | `03` | **Running** | car was READY ✓ |

**B048 enum (verbatim from firmware):** `0x0`=ConnectedNotCharging, `0x1`=Idle,
`0x3`=Running, `0x6`=Charging(AC/Type2), `0x7`=CcsCharging(DC), `0x8`=AboutToSleep,
`0xa`=Connected, `0xc`=StartingCharge.

**SOC calibration nuance:** B046 is not a plain ÷10. MG normalises the raw between
depth-of-discharge limits `lo`/`hi` (config `xmg/bms.dod.lower|upper`,
vehicle-specific). Our ÷10 assumes lo=0, hi=1000; the true MAXUS lo/hi must be
**fitted from B046 readings at known high and low SOC**.

## Two hypotheses this OVERTURNS
1. **B05C is coolant temp, not SOC.** We had B05C as a SOC candidate (`05-…`); the
   reference + our 30.5°C decode show it is the **battery coolant temperature**. The
   143→141 drift we saw was the coolant cooling, not SOC discharge.
2. **SOC is B046, not B05C.** (And B04A, once a "SOC candidate", is a main-relay
   status — see bonus below.)

## Bonus PIDs identified (unverified leads)
- `B045` batteryResistance (our raw `3F FF` = likely max/invalid at rest)
- `B047` batteryError (our `00` = no error)
- `B049`/`B04A`/`B052` main relay status B/G/P (contactors)
- `B0CE` bmsRange (estimated range)
- `B06D` bmsTime — our raw `1A 07 10 0B 31 08` decodes as **2026-07-16 11:49:08**
  (Y/M/D H:M:S), a BMS real-time clock.

## Trust tiers (we do NOT adopt MG blindly — different vehicle)
1. **Proven:** MG formula == our confirmed formula (the 6 PIDs above).
2. **Strong lead (independent ref matches):** SOC B046 vs dash 50–55%.
3. **Lead, needs a discriminator:** SOH B061 (no dash-SOH reference yet); coolant
   B05C (must rise under thermal load to prove it's coolant, not a static temp).

## Verdict
- No hypothesis is marked `confirmed` from this reference alone. H1/H2/H4/H6 are
  **re-pointed with concrete formulas and elevated to `testing`**. `pids.py` is left
  unchanged until live verification on MAXUS.

## Next test at the vehicle (consolidated checklist — one ELM327 session)
- [ ] **SOC (B046):** read at a **high** and a **low** SOC (near full / near empty)
      to fit `lo`/`hi`; check it tracks the dash across the range.
- [ ] **SOH (B061):** obtain a dash/dealer SOH % and compare to INT16/100.
- [ ] **Coolant (B05C):** watch it **rise** during sustained drive / DC charge
      (delta test) to confirm it's coolant, not battery temp.
- [ ] **BMS status (B048):** read while **AC-charging** (expect `06`) and
      **DC-charging** (expect `07`).
- [ ] **Scan VCU 7E3** (capture-enabled) for the motor/inverter (radiator) loop temp.
