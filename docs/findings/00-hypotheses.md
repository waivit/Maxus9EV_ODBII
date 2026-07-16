# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H0 | `uds.parse` fails on 11-bit CAN (3-char id) → all "no data"; a 3-char-id rewrite fixes it. | confirmed | `03-uds-parser-11bit-header.md`, `04-pack-current-sign-reversed.md` — bug seen live, fix coded, offline regression green, **live re-verify at the car: all PIDs decode correctly, user-confirmed 2026-07-16**. Was blocking H1/H2/H4. |
| H1 | SOC lives on a `B0xx` PID on header 7E5; leading candidate `B05C` with scale `byte × ~0.4`. | testing | `05-soc-candidate-b05c.md` — dash 50–55 % vs `B05C=141` → `byte×~0.4` family fits (55–56 %), `÷2` (70.5 %) ruled out. Mid-scale point can't pin exact scale; re-test at high/low SOC after drive+charge. (Ex-look-alike `B0E3=142` now judged a **temperature** (31 °C), not SOC — see `06`, so B05C is the sole SOC lead.) |
| H2 | ~~SOH is `B04C`~~ → **B04C ruled out** (live raw = `03`, not a health %). True SOH PID unknown. | testing | `02-soh-formula.md` |
| H3 | Regen level is not exposed via OBD2 (Service 22); it rides the infotainment CAN bus. Needs a passive sniff. | parked | ESP32 + TJA1050 passive CAN sniff. Deprioritized by decision. |
| H4 | Charge Status `B048` on 7E5 is an enum. | testing | Live `B048=03` confirmed (2026-07-16). Still need values across states (charge/discharge/idle). |
| H5 | Pack Current sign convention is reversed: **positive = discharging, negative = charging** (opposite of `pids.py`'s comment). | confirmed | `04-pack-current-sign-reversed.md` — controlled AC-load experiment (idle→load up→load down), current tracked load direction, not charging. **Fix applied 2026-07-16:** flipped `battery_state()` in `diag.py` (reused by `webdash.py`) + `pids.py` comment; `test_diag.py` guards the mapping. |
| H6 | A coolant / radiator temperature is exposed on a BMS (7E5) PID. | testing | `06-coolant-temp-candidates.md` — offline decode `byte/2 − 40` of the capture gives candidates (`B0E3`=31 °C; module-record `byte[6]`=39–44 °C; `B0D6`), but an **idle snapshot can't isolate coolant temp** (all ~30 °C). Next: live delta test as thermal load rises; and scan **VCU 7E3** (radiator/motor loop likely there, not on BMS). |

## Open questions to resolve along the way

- Full `B04x` range on 7E5 has not been exhaustively scanned — SOC may sit on an
  untried PID next to the ones already probed.
- Is SOC perhaps not a single byte but derived (e.g. from a coulomb-count or a
  voltage lookup) and simply not published over UDS at all?
- What are the discrete values of Charge Status `B048` across states
  (off / READY-idle / driving / AC charge / DC charge)?
- Which PID actually holds SOH? `B04C` is ruled out (raw = 3). Re-scan a wider
  range and correlate against the dash battery-health readout.
- Confirm `B05C` is SOC and pin its scale by reading it at high/low SOC (near
  full/empty), not just mid-scale.
- Which PID carries coolant / thermal-loop temperature (H6)? Battery-loop
  candidates are on 7E5 (note 06); the motor/inverter (radiator) loop is likely on
  VCU 7E3, which hasn't been scanned yet.

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and VCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends. See also
`04-pack-current-sign-reversed.md` for the live re-verification of the parser fix
(H0) and the Pack Current sign discovery (H5).
