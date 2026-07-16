# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H0 | `uds.parse` fails on 11-bit CAN (3-char id) → all "no data"; a 3-char-id rewrite fixes it. | confirmed | `03-uds-parser-11bit-header.md`, `04-pack-current-sign-reversed.md` — bug seen live, fix coded, offline regression green, **live re-verify at the car: all PIDs decode correctly, user-confirmed 2026-07-16**. Was blocking H1/H2/H4. |
| H1 | SOC lives on a `B04x` PID on header 7E5. `B040`/`B044` return NRC 0x31. | open | Live B040–B060 scan (2026-07-16) — new candidates: `B05C=0x8F=143` (÷2 = **71.5 %**, best lead), `B046=022D`, `B045=3FFF` (sentinel?). `B04A=00`, `B04B=00`. |
| H2 | ~~SOH is `B04C`~~ → **B04C ruled out** (live raw = `03`, not a health %). True SOH PID unknown. | testing | `02-soh-formula.md` |
| H3 | Regen level is not exposed via OBD2 (Service 22); it rides the infotainment CAN bus. Needs a passive sniff. | parked | ESP32 + TJA1050 passive CAN sniff. Deprioritized by decision. |
| H4 | Charge Status `B048` on 7E5 is an enum. | testing | Live `B048=03` confirmed (2026-07-16). Still need values across states (charge/discharge/idle). |
| H5 | Pack Current sign convention is reversed: **positive = discharging, negative = charging** (opposite of `pids.py`'s comment). | confirmed | `04-pack-current-sign-reversed.md` — controlled AC-load experiment (idle→load up→load down), current tracked load direction, not charging. Fix pending: flip `battery_state()` in `diag.py`/`webdash.py` + `pids.py` comment (separate iteration). |

## Open questions to resolve along the way

- Full `B04x` range on 7E5 has not been exhaustively scanned — SOC may sit on an
  untried PID next to the ones already probed.
- Is SOC perhaps not a single byte but derived (e.g. from a coulomb-count or a
  voltage lookup) and simply not published over UDS at all?
- What are the discrete values of Charge Status `B048` across states
  (off / READY-idle / driving / AC charge / DC charge)?
- Which PID actually holds SOH? `B04C` is ruled out (raw = 3). Re-scan a wider
  range and correlate against the dash battery-health readout.
- Confirm `B05C` (=143) is SOC by watching it change across a charge/drive cycle.

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and VCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends. See also
`04-pack-current-sign-reversed.md` for the live re-verification of the parser fix
(H0) and the Pack Current sign discovery (H5).
