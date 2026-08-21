# 12 — Identifying the 33 ECUs, and structure found inside the BMS DIDs

- **Date:** 2026-08-21
- **Hypothesis under test:** none directly — this is desk analysis of the capture
  from `11-native-can-ecu-scan.md`. It corrects the naming of `7E3`, expands the
  sentinel list, and adds structure to already-confirmed PIDs.
- **Setup:** offline. Input is the two `MODE_SCAN` captures (33 ECUs, 1824 DIDs);
  analysis via the new `tools/analyze_scan.py`.

## What I did
Parsed both scan captures and (a) read the **standard ISO 14229 identification
DIDs** on every ECU, and (b) applied the project's known formulas to every short
BMS payload to look for structure. Deliberately separated what the standard
*defines* from what a plausible-looking number merely *suggests*.

## Raw artifacts

### ECU identification — from standardised DIDs
`F183` (part number), `F192` (supplier hardware), `F194` (supplier software) and
`F18B` (build date, BCD) are defined by ISO 14229, so reading them is not
guesswork. Interpreting the resulting strings as a module name still is.

| ECU | `F183` | `F192` | `F194` | build | reading |
| --- | --- | --- | --- | --- | --- |
| `7E5` | `BMS !..100` | `MFAC5500B0` | `43A1354MR4` | 2023-06-07 | **BMS** — names itself |
| `7E3` | `IMCU!.'130` | — | `MIFAMCU520` | 2023-05-26 | **IMCU** — motor control unit |
| `7E4` | `CCU_S001R3` | `H100R0` | `S200R0` | 2023-05-21 | **CCU** — central control unit |
| `704` | `CB313MX100` | — | `BDUMFEV707` | 2022-08-08 | **BDU** — battery disconnect unit |
| `747` | `AVM1223118` | `MFAVMV1.02` | `V1.44V1.09` | (invalid) | **AVM** — around-view monitor |
| `730` | `SCM0323V05` | `SCM:HW1.00` | `SWV3.81107` | (invalid) | **SCM** |
| `775` | `EP2B2.1.02` | `H06` | `SOPA3.1.06` | 2023-05-12 | |
| `716` | `MIFAFBLV04` | `45Y-00-01` | `EPX1-SW-06` | 2023-05-26 | |
| `709` | `CBLMIFA310` | `CGM83B1L01` | `LSAMFEV700` | 2023-04-13 | |
| `75A` `75B` `75C` `75D` | `01.00.1300` | `B20` | `70.03.00` | Oct–Dec 2022 | **four identical modules** |
| `732` `733` | `B00A29MIFA` | *(differs)* | `A00A52MIFA` | May 2023 | **identical pair** |
| `715` `724` | `1020210302` | *(differs)* | `865373_ILNo…` / `870903_Bw3V…` | 2023 | build-hash style software ids |
| `7F1` | `MIFA.04.30` | `..A013Y200` | `56960R0851` | 2022-12-29 | |

Most build dates cluster in **April–June 2023**, consistent with VIN position 10
(`P` = 2023). Several modules return junk in `F18B` (`2030-30-31`, `2000-00-00`,
`FF FF FF`) — the DID exists but is not populated, so the tool flags rather than
prints those.

### Structure found in the BMS payloads
```
B056 = 94 06        0x94 = 148 -> 34.0 C   trailing 06
B057 = 92 0B        0x92 = 146 -> 33.0 C   trailing 0B
B058 = 0E 1C 1A     0x0E1C     -> 3.612 V  trailing 1A (= 26)
B059 = 0E 06 51     0x0E06     -> 3.590 V  trailing 51 (= 81)
```
```
B0F7 = 9C 40        = 40000 exactly
B074 = FF FF FF FE      B0BD = FF FE
B0EE = FF FF FF FE      B0EF = FF FF FF FE
```
```
F18B = 23 06 07     BCD -> 2023-06-07
F183 = 42 4D 53 20 ...  ASCII -> "BMS "
F192 = 4D 46 41 43 ...  ASCII -> "MFAC5500B0"
```
Of the BMS's 163 DIDs, **86 carry data**; 77 are all-`00` or all-`FF`.

## Observations
- **`7E3` calls itself `IMCU`**, and its software id is `MIFAMCU520`. Every PID we
  have confirmed on it is motor-related (`B405` motor temp, `B402` motor speed,
  `B401` torque), which agrees.
- **`B056`/`B057` return two bytes, not one.** The trailing byte follows the same
  shape as `B058`/`B059`, where the trailing byte is the **cell index** — so it
  reads as a **sensor index**: temp 34.0 °C from sensor 6, 33.0 °C from sensor 11.
- **`B0F7` is exactly 40000**, which is precisely the offset in our confirmed Pack
  Current formula `(INT16 − 40000) / 40`.
- **Four sentinel DIDs join the two already known.** `B045` and `B0CE` were
  recorded in note 10; `B074`, `B0BD`, `B0EE`, `B0EF` show the same `FFFE` /
  `FFFFFFFE` pattern.
- Four modules (`75A`–`75D`) are byte-identical in part, hardware and software.

## Interpretation
The identification DIDs turn "33 unknown addresses" into a partly-named map, and
they do it from a **standardised** source rather than pattern-matching — which is
why this is worth acting on where the numeric guesses below are not.

**`7E3` is the motor controller, not a general vehicle control unit.** Our docs
have called it "VCU" since `01-confirmed-pids.md`, a name inherited from the MG
cross-reference (note 09), not from this vehicle. The ECU's own identification and
the PIDs it serves both say IMCU.

Four identical modules most plausibly means **one per door**, and an identical
pair means a left/right or front/rear pairing — but nothing here proves it; that
needs a state diff (operate one door, re-scan, see which address moves).

`B0F7 = 40000` matching the current-sensor offset exactly is unlikely to be a
coincidence, and reads as the **sensor's zero-point calibration constant**. It is
a strong lead, not a confirmed decode — a constant does not change, so a state
diff cannot confirm it; it would need corroboration from a reference.

## Verdict
- **Confirmed (standard DIDs, high confidence):** `7E5` = BMS, `7E3` = **IMCU**,
  `7E4` = CCU, `704` = BDU, `747` = AVM. `F18B` decodes as a BCD build date.
- **Correction:** rename `7E3` from "VCU" to **IMCU** in the docs. The address,
  response id (`7EB`) and every confirmed formula on it are unchanged — only the
  name was wrong.
- **Structure confirmed:** `B056`/`B057` carry a **sensor index** in byte 1, the
  same way `B058`/`B059` carry a cell index. `poc4` decodes the temperature
  correctly; it simply discards the index.
- **Sentinels expanded** to `B045`, `B0CE`, `B074`, `B0BD`, `B0EE`, `B0EF`.
- **Strong lead:** `B0F7` = current-sensor zero offset (40000).
- **Unresolved and not guessable from one snapshot:** the 4-byte counters
  (`B076`–`B07F`, `B095`, `B0A0`), the `B072`/`B073` pair, and the 77 blank DIDs.
  A plausible-looking decode is not evidence and none is recorded as a finding.

## Next
1. **Regen hunt (H3, primary goal):** scan at regen Low / Medium / High and diff.
   This is also the method that will resolve most of the "unresolved" items above.
2. Re-scan while **driving** and while **charging** and diff against this parked
   baseline — the counters and blank DIDs should separate into meaningful groups.
3. Identify the unnamed modules by operating a specific feature (a door, a seat)
   between two scans and seeing which address changes.
4. Surface the sensor/cell index bytes on the dashboard instead of discarding them.
