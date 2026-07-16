# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H0 | `uds.parse` fails on 11-bit CAN (3-char id) → all "no data"; a 3-char-id rewrite fixes it. | confirmed | `03-uds-parser-11bit-header.md`, `04-pack-current-sign-reversed.md` — bug seen live, fix coded, offline regression green, **live re-verify at the car: all PIDs decode correctly, user-confirmed 2026-07-16**. Was blocking H1/H2/H4. |
| H1 | **SOC = `B046`** (7E5), formula `(raw−lo)×100/(hi−lo)`; `÷10` ≈ 55.7 % as first approx. | testing | `07-mg-ev-reference.md` — MG/SAIC ref. Our `B046=022D=557` → ≈55.7 %, matches dash 50–55 %. lo/hi need fitting at high/low SOC. (Old `B05C` SOC guess withdrawn — B05C is coolant, see H6.) |
| H2 | **SOH = `B061`** (7E5), formula `INT16/100`. | testing | `07-mg-ev-reference.md` — MG/SAIC ref. Our `B061=2512=9490` → 94.9 %. Needs a dash/dealer SOH to confirm. (`B04C` ruled out earlier, `02-soh-formula.md`.) |
| H3 | Regen level is not exposed via OBD2 (Service 22); it rides the infotainment CAN bus. Needs a passive sniff. | parked | ESP32 + TJA1050 passive CAN sniff. Deprioritized by decision. |
| H4 | Charge Status `B048` on 7E5 is a BMS-status enum. | testing | `07-mg-ev-reference.md` — MG enum: `00`=ConnectedNotCharging, `01`=Idle, `03`=Running, `06`=Charging(AC), `07`=CcsCharging(DC), `08`=AboutToSleep, `0a`=Connected, `0c`=StartingCharge. Our live `B048=03`=Running (was READY) ✓. Verify `06`/`07` while charging. |
| H5 | Pack Current sign convention is reversed: **positive = discharging, negative = charging** (opposite of `pids.py`'s comment). | confirmed | `04-pack-current-sign-reversed.md` — controlled AC-load experiment (idle→load up→load down), current tracked load direction, not charging. **Fix applied 2026-07-16:** flipped `battery_state()` in `diag.py` (reused by `webdash.py`) + `pids.py` comment; `test_diag.py` guards the mapping. |
| H6 | **Battery coolant temp = `B05C`** (7E5), formula `byte×0.5−40`. | testing | `07-mg-ev-reference.md` — MG names B05C `batteryCoolantTempPid`. Our `B05C=8D=141` → 30.5 °C (= idle ambient). Confirm via delta test (must rise under thermal load). Motor/radiator loop temp likely on **VCU 7E3** (unscanned). Candidates in `06-coolant-temp-candidates.md`. |

## Open questions to resolve along the way

Most discovery questions are now answered by the MG/SAIC reference (`07-…`); what
remains is **live verification on MAXUS** (see the checklist in note 07):
- **SOC (B046):** fit the `lo`/`hi` depth-of-discharge limits by reading at known
  high and low SOC; confirm it tracks the dash across the range.
- **SOH (B061):** get a dash/dealer SOH % to confirm `INT16/100`.
- **Coolant (B05C):** confirm it *rises* under thermal load (vs a static battery temp).
- **BMS status (B048):** confirm `06`/`07` appear while AC/DC charging.
- **Radiator/motor loop temp:** scan **VCU 7E3** — not on the BMS.
- Confirm the bonus PIDs (B045 resistance, B047 error, B049/B04A/B052 relays,
  B0CE range, B06D RTC) behave as the reference suggests.

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and VCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends. See also
`04-pack-current-sign-reversed.md` for the live re-verification of the parser fix
(H0) and the Pack Current sign discovery (H5).
