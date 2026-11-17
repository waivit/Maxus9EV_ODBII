# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H0 | `uds.parse` fails on 11-bit CAN (3-char id) → all "no data"; a 3-char-id rewrite fixes it. | confirmed | `03-uds-parser-11bit-header.md`, `04-pack-current-sign-reversed.md` — bug seen live, fix coded, offline regression green, **live re-verify at the car: all PIDs decode correctly, user-confirmed 2026-07-16**. Was blocking H1/H2/H4. |
| H1 | **SOC = `B046`** (7E5): `INT16/10` = **Raw SOC**; the dash ≈ **Raw + ~4 %** (MAXUS gain ~1.03). | testing (all but locked) | `10-poc4-live-crosscheck.md` + `08-soc-b046-live.md` + `09-ovms-cross-reference.md`. **Settled** re-measurement: 452@49 %, **752@80 %** (the old 572@80 % was a **stale post-charge read — staleness theory confirmed**). dash−bridge = +3.8 / +4.8 → `Display ≈ 1.033×Raw + 2.3` (gain ~1.03, **not** MG's 1.1 → BMS-version-specific). Only a **low-SOC (~20–30 %)** point is left to lock `Raw+4` vs linear; formula already ~±1 %. `005B` returns `--` on MAXUS (dead, not a SOC source). |
| H2 | **SOH = `B061`** (7E5), formula `INT16/100`. | testing | `07-mg-ev-reference.md` + `10-poc4-live-crosscheck.md`. Two reads: `B061=2512`→94.9 % and `B061=24D6=9430`→94.3 % (consistent). Still needs a dash/dealer SOH to confirm absolute accuracy. (`B04C` ruled out earlier, `02-soh-formula.md`.) |
| H3 | **Regen level = `B480`** (7E3/IMCU): `00`=Low, `01`=Medium, `02`=High. | **confirmed** ⭐ | `13-regen-level-found.md` — **the project's primary goal, met.** Three DID scans taken at Low/Medium/High: of **309** DIDs read in all three, 31 changed at least once, 5 differed in all three, and **exactly one stepped `00`→`01`→`02` in selector order**. The other four either count with time or break the ordering — a two-level diff would not have separated them. Verified live by predicting "Medium" from a raw `01` without being told the setting, confirmed against the car. Sits on the **motor control unit** in the same `B4xx` page as the confirmed `B401`/`B402`/`B405`. *(Note 11's "would need an internal bus tap" was wrong — it assumed sniffing was the only route.)* **Read side: confirmed.** **Write side: closed — `B480` is read-only** (`14-regen-write-attempt.md`): an Extended Diagnostic Session **does** open (`10 03` → `50 03`), and Service `2E` becomes available there (NRC moves `0x7F` → `0x31`), but the ECU then refuses the DID itself with `requestOutOfRange` at **all three** levels. **`0x33` never appeared** — not a security lock, not a session problem, not a vehicle-state problem: `B480` mirrors the selector rather than storing it. Writing regen over OBD would need whichever DID/ECU actually owns the value, which may not be reachable from this gateway-isolated port at all. **Shadow mode** (read live, advise, let the driver select) remains fully available. **No other writable store exists either** (`15-regen-store-search.md`): sweeping **all 1799 DIDs on 33 ECUs** at Low/Medium/High found 128 changes, of which only `B480` steps `00`→`01`→`02` in order — the other four single-byte candidates were counters or failed the middle step. Setting regen via OBD is closed; the only route left is tapping the **internal** CAN bus the infotainment screen uses. |
| H4 | Charge Status `B048` on 7E5 is a BMS-status enum. | testing | `07-mg-ev-reference.md` + `10-poc4-live-crosscheck.md` — MG enum: `00`=ConnectedNotCharging, `01`=Idle, `03`=Running, `06`=Charging(AC), `07`=CcsCharging(DC), `08`=AboutToSleep, `0a`=Connected, `0c`=StartingCharge. Our live `B048=03`=Running seen again while driving (READY) ✓. Verify `06`/`07` while charging. |
| H5 | Pack Current sign convention is reversed: **positive = discharging, negative = charging** (opposite of `pids.py`'s comment). | confirmed | `04-pack-current-sign-reversed.md` — controlled AC-load experiment (idle→load up→load down), current tracked load direction, not charging. **Fix applied 2026-07-16:** flipped `battery_state()` in `diag.py` (reused by `webdash.py`) + `pids.py` comment; `test_diag.py` guards the mapping. |
| H7 | **Inspection report over ELM327 (`firmware/poc6_report`)**: (a) a report snapshot (SOH `B061`, SOC `B046`, cell max/min `B058`/`B059` + cell index, temp hi/lo `B056`/`B057` + sensor index, odometer `E101`) taken through the ESP32 → BT → ELM327 path shows the **same values as the poc4 dashboard** read at the same moment; (b) the new on-device ISO-TP reassembly decodes the 12 module groups (`B001`…`B0CB`, 10-byte multi-frame replies) into 2 voltages + 2 temperatures each. | testing | Motivated by hub handoff **H-0135** (taladkrub wants a JPG battery report; Design: no VIN, no good/bad colouring). Prior evidence: ELM327 auto flow-control already returned multi-frame replies on 2026-07-16 (`captures/20260716_104946_7E5_B0_scan061-0FF.jsonl`: `B0A3` = `7ED 10 0A …` + `7ED 21 …`; `B0EC` = 403 bytes) — so the transport is proven and only the **ESP32 parser** is new. Module formulas (`V = INT16/2000 + 1.0`, `T = byte×0.5 − 40`) are the MG ones and remain **unverified** — dashboard-only, not on the report image. Desk check is a boot-time parser self-test on captured vectors = **regression guard, not verification**. Live test = user at the car: generate a report, compare with the dashboard tab. |
| H6 | **Battery coolant temp = `B05C`** (7E5), formula `byte×0.5−40`. | testing | `07-mg-ev-reference.md` + `10-poc4-live-crosscheck.md` — MG names B05C `batteryCoolantTempPid`. Live `B05C=91=145` → 32.5 °C, but **exactly equals Batt Temp Hi** → still needs a **thermal-delta** test (warm pack) to prove it is a distinct coolant sensor, not just tracking cell temp. Motor/radiator loop is on IMCU 7E3: `B405` motor temp now confirmed; `B309` (MG's VCU-coolant) returns `--` (absent on MAXUS). |

## Open questions to resolve along the way

Most discovery questions are now answered by the MG/SAIC reference (`07-…`); what
remains is **live verification on MAXUS** (see the checklist in note 07):
- **SOC (B046):** fit the `lo`/`hi` depth-of-discharge limits by reading at known
  high and low SOC; confirm it tracks the dash across the range.
- **SOH (B061):** get a dash/dealer SOH % to confirm `INT16/100`.
- **Coolant (B05C):** confirm it *rises* under thermal load (vs a static battery temp).
- **BMS status (B048):** confirm `06`/`07` appear while AC/DC charging.
- **Radiator/motor loop temp:** scan **IMCU 7E3** — not on the BMS.
- Confirm the bonus PIDs (B045 resistance, B047 error, B049/B04A/B052 relays,
  B0CE range, B06D RTC) behave as the reference suggests.

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and IMCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends. See also
`04-pack-current-sign-reversed.md` for the live re-verification of the parser fix
(H0) and the Pack Current sign discovery (H5).

**New (poc4 live test, `10-poc4-live-crosscheck.md`, 2026-08-14):** four IMCU (7E3)
PIDs proven by idle→driving delta — `B405` motor temp (`b[0]−40`), `B402` motor
speed (`INT16−0x7FFF` rpm), `B401` motor torque (`INT16−0x7FFF`), `E101` odometer
(3-byte km).

**Dead on MAXUS (returned `--`, DID absent — remove/hide these leads):** `B309`,
`005B`, `0046`, `B900`, `B18C`, `B703`, `BA40`, `BA00`, `B712`, `000D`. Not a
header issue — other 7E3 DIDs answered in the same cycle. **Sentinel garbage to
filter:** `B045` (raw `3FFF`), `B0CE` (raw `FFFE`).

**Native CAN scan (`11-native-can-ecu-scan.md`, 2026-08-20):** the diagnostic
address range holds **33 responding ECUs** (we knew 3) and **1824 readable DIDs**
— `7E3` IMCU 219, `7E5` BMS 163, `7E4` CCU 90. Request→response offsets are
**not** the OBD-standard `+8` (`+0x80`, `+0x70` and `+0x08` all occur), so any
scan must accept whatever id replies. Every ECU answers `F183` (part number) and
`F194` (software version) — the practical way to identify unknown modules.
**Per-cell module data is readable as single frames** at `B001 B009 B011 B019
B021 B029 B0A3 B0AB B0B3 B0BB B0C3 B0CB` (12 groups, stride 8), contradicting
note 09's expectation that it needed multi-frame. `B058` byte 3 is the cell
index. Tooling gap: NRC `0x78` (responsePending) is treated as final, so those
DIDs are **unread, not unsupported**.
