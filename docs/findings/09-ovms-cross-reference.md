# 09 — OVMS / MG-community cross-reference: full PID map + the two-layer SOC

- **Date:** 2026-07-17
- **Hypothesis under test:** cross-references H1 (SOC), H2 (SOH), H6 (coolant);
  supplies new leads for VCU temps and many unqueried PIDs.
- **Setup:** offline desk research. Deliberately **four independent sources** —
  note 07 used only the OVMS firmware, and a single source proved insufficient.

## Sources (independent of each other)
1. **OVMS firmware** — `vehicle_mgev`: `mg_obd_pids.h` (DIDs),
   `mg_poll_bms.cpp` + `mg_poll_vcu.cpp` (decode formulas).
2. **mgevs.com community RE thread** — "ZS EV Reverse Engineering CAN and OBD
   Spreadsheets" (independent owner reverse-engineering).
3. **Torque Pro PID CSV** — `github.com/VinceW31/MGZSEV-UK-Torque-Pro-PIDS`
   (formulas actually in daily use by owners).
4. **OVMS RE spreadsheet** / MG EV docs (`docs.openvehicles.com`).

## The headline: SOC is **two layers**, and that resolves our mismatch

| Layer | Source of truth | Formula |
| --- | --- | --- |
| **Raw SOC** | `22 B0 46` | `INT16 / 10` |
| **Display SOC** (what the dash shows) | *derived from Raw* | Torque: `1.1 × (Raw + BMS_version_factor)`, clamped 0–100 |

- **All three of** OVMS, mgevs and the Torque CSV agree `B046 ÷ 10` = **Raw SOC**.
  The Torque CSV literally names them `Raw_SOC` and `Display_SOC`.
- The Torque README states the SOC correction **depends on the detected BMS
  software version** (`BMS_Version_SoC_Factor`).
- ⇒ Our reading was **the right PID with the wrong (missing) transform**. What we
  lack is the **per-vehicle/per-BMS-version calibration**, not a different PID.
- ⚠️ Our own two points still don't fit even this shape (implied gain 1.08 @49 %
  vs 1.40 @80 %), so the **80 % sample is suspect** — most likely Raw had not yet
  updated right after charging. See `08-soc-b046-live.md`.
- `005B` (`socPid`, `data[0]×100/255`) exists in the OVMS firmware but is **not**
  what the MG community uses for the dash SOC → treat as a **weak secondary lead**,
  worth one opportunistic read.

## Cross-referenced PID map

`INT16 = data[0]×256 + data[1]`. **Confirmed** = already proven on our MAXUS.

### BMS — our header `7E5`
| DID | Metric | Formula | Status |
| --- | --- | --- | --- |
| `B041` | Bus Voltage | `INT16/4` | confirmed |
| `B042` | Pack Voltage | `INT16/4` | confirmed |
| `B043` | Pack Current | `(INT16−40000)/40` | confirmed (H5 sign) |
| `B056` | Batt Temp | `data[0]×0.5−40` | confirmed |
| `B057` | Batt Temp Low | `data[0]×0.5−40` | confirmed |
| `B058`/`B059` | Cell V max/min | `INT16/1000` | confirmed |
| `B048` | BMS status enum | see note 07 | testing (H4) |
| `B046` | **Raw SOC** | `INT16/10` | testing (H1) |
| `B061` | SOH | `INT16/100` | testing (H2) |
| `B05C` | Coolant temp | `data[0]×0.5−40` | testing (H6) |
| `B045` | Batt resistance | `INT16/2` | new lead |
| `B047` | Battery error | `data[0]` | new lead |
| `B0CE` | BMS range | scale unknown | new lead |
| `B049`/`B04A`/`B052` | Main relay B/G/P | `data[0]` | new lead |
| `B06D` | BMS RTC | Y/M/D H:M:S bytes | new lead |
| `005B` | SOC (generic) | `data[0]×100/255` | weak lead |
| `0046` | Ambient temp | `data[0]−40` | new lead |
| `00A6` | Odometer | 4-byte `/10` | new lead |
| `000D` | Vehicle speed | `data[0]` | new lead |
| `B001`,`B009`,`B011`,`B019`,`B021`,`B029`,`B0A3`… | **Per-cell stats** | V: `INT16/2000+1.0`; T: `data[1]×0.5−40` | new lead — **multi-frame** |
| `F194` | **BMS software version** | ASCII/bytes | new lead — **multi-frame**, key to the SOC transform |

### VCU — our header `7E3`
| DID | Metric | Formula |
| --- | --- | --- |
| `0112` | 12 V supply | MG: `data[0]/10` — **ours is `data[0]×1`** (see conflicts) |
| `B405` | **Motor temp** | `data[0]−40` |
| `B309` | **VCU coolant temp** | `data[0]−40` |
| `B402` | Motor speed | `INT16−0x7FFF` RPM |
| `B401` | Motor torque | `INT16−0x7FFF` |
| `B712` | Charge rate | `INT16/10` kW |
| `B900` | Gear | `8`=Park, `0`=Reverse, else Drive |
| `B18C` | Ignition state | `data[0]` |
| `B703` | HV contactor | `data[0] & 1` |
| `BA00` | Vehicle speed | `(INT16−20000)/100` kph |
| `BA40` | Handbrake | `data[0]` |
| `E101` | Odometer | 3-byte km |
| `F190` | VIN | ASCII — **multi-frame** |

### DCDC (VCU-side `B580`–`B587`)
mode `data[0]`; I-in `(INT16−0x7FFF)/10` A; V-in `INT16/10` V;
I-out `INT16/10` A; V-out `INT16/10` V; temp `data[0]−40` °C.

## ⚠️ Conflicts found — MG formulas are NOT blindly portable
1. **12 V (`0112`):** MG decodes `data[0]/10`; **our confirmed MAXUS formula is
   `data[0]×1`** (captured vector `7EB 04 62 0112 0E` → `0x0E`=14 → **14.0 V**).
   MG's would give 1.4 V. Ours is live-proven — keep ours.
2. **Temperature scaling differs by ECU:** BMS temps use `×0.5−40`, but VCU temps
   (`B405`, `B309`) use `−40` with no ×0.5. Do not assume one scale everywhere.
3. Source 1 alone implied `005B` was the dash SOC; sources 2–4 show it is not.

## Interpretation
The SAIC platform decode transfers well for the **already-confirmed** PIDs (all six
match exactly), which is why the map is trustworthy as a **lead generator**. But the
three conflicts above prove per-vehicle verification is mandatory — MAXUS 9 is not
an MG ZS EV. Every row marked "new lead" stays `testing` until read on our car.

## Verdict
- **H1 refined, not rejected:** `B046÷10` **is** Raw SOC (3 independent sources).
  What is missing is the **Display-SOC transform** for our BMS version. See the
  follow-up section in `08-soc-b046-live.md`.
- H2/H6 unchanged (`testing`), now with independent corroboration of the formulas.
- Large new lead set added above, all `testing`.

## Next
1. **Multi-point SOC capture** (dash 30/50/70/90 % with Raw `B046`) → fit the
   MAXUS transform; recheck the suspect 80 % point for staleness.
2. **VCU `7E3`**: read `B405` / `B309` — this finally answers note 07's open
   "motor/radiator loop temp" question.
3. **Multi-frame (ISO-TP) support** is the highest-value capability next: it unlocks
   per-cell voltages, VIN, and **`F194` BMS software version** — the value the SOC
   transform is keyed on. Our `uds.c` currently handles single-frame only.
