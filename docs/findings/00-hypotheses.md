# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H0 | `uds.parse` fails on 11-bit CAN (3-char id) → all "no data"; a 3-char-id rewrite fixes it. | confirmed | `03-uds-parser-11bit-header.md`, `04-pack-current-sign-reversed.md` — bug seen live, fix coded, offline regression green, **live re-verify at the car: all PIDs decode correctly, user-confirmed 2026-07-16**. Was blocking H1/H2/H4. |
| H1 | SOC lives on a `B0xx` PID on header 7E5; leading candidate `B05C` with scale `byte × ~0.4`. | testing | `05-soc-candidate-b05c.md` — dash 50–55 % vs `B05C=141` → `byte×~0.4` family fits (55–56 %), `÷2` (70.5 %) ruled out. Mid-scale point can't pin exact scale; re-test at high/low SOC after drive+charge. Look-alike `B0E3=142`. |
| H2 | ~~SOH is `B04C`~~ → **B04C ruled out** (live raw = `03`, not a health %). True SOH PID unknown. | testing | `02-soh-formula.md` |
| H3 | Regen level is not exposed via OBD2 (Service 22); it rides the infotainment CAN bus. Needs a passive sniff. | parked | ESP32 + TJA1050 passive CAN sniff. Deprioritized by decision. |
| H4 | Charge Status `B048` on 7E5 is an enum. | testing | Live `B048=03` confirmed (2026-07-16). Still need values across states (charge/discharge/idle). |
| H5 | Pack Current sign convention is reversed: **positive = discharging, negative = charging** (opposite of `pids.py`'s comment). | confirmed | `04-pack-current-sign-reversed.md` — controlled AC-load experiment (idle→load up→load down), current tracked load direction, not charging. Fix pending: flip `battery_state()` in `diag.py`/`webdash.py` + `pids.py` comment (separate iteration). |
| H6 | A coolant / radiator temperature is exposed on a BMS (7E5) PID. | open | Start **offline** from `captures/20260716_104946_7E5_B0_scan061-0FF.jsonl`: apply the known temp decode `byte/2 − 40` (from B056/B057) to the scan; ~0x8C bytes ≈ 30 °C. Several `…8C 8C…` patterns seen (B0A3/B0AB/B0B3…). Then verify live as the thermal loop heats up. |

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
- Which PID carries coolant / thermal-loop temperature (H6), and are the
  battery-loop and motor/inverter-loop temps separate PIDs?

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and VCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends. See also
`04-pack-current-sign-reversed.md` for the live re-verification of the parser fix
(H0) and the Pack Current sign discovery (H5).
