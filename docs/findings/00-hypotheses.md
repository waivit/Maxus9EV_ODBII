# Hypotheses Register

Discovery loop: **research → hypothesize → test → verify**, repeat. Update the
status of each hypothesis as evidence arrives. Link each result to a numbered note
(`NN-topic.md`) built from `TEMPLATE.md`.

Status legend: `open` (untested) · `testing` · `confirmed` · `rejected` ·
`blocked` · `parked` (deprioritized on purpose).

| ID | Hypothesis | Status | Evidence / note |
| --- | --- | --- | --- |
| H1 | SOC lives on a `B04x` PID on header 7E5. `B040` returns NRC 0x31 (requestOutOfRange); candidate `B04A` returns 0. | open | Scan `B041`–`B07F` on 7E5 (`scanner.py` option 1). See dead ends in `01-confirmed-pids.md`. |
| H2 | SOH is `B04C` on 7E5 but the current formula is wrong (reads 1.5%). | testing | `02-soh-formula.md` |
| H3 | Regen level is not exposed via OBD2 (Service 22); it rides the infotainment CAN bus. Needs a passive sniff. | parked | ESP32 + TJA1050 passive CAN sniff. Deprioritized by decision. |
| H4 | Charge Status `B048` on 7E5 is an enum (observed value 3 = driving?), not fully decoded. | open | Correlate `B048` against Pack Current sign (charge vs discharge vs idle). |

## Open questions to resolve along the way

- Full `B04x` range on 7E5 has not been exhaustively scanned — SOC may sit on an
  untried PID next to the ones already probed.
- Is SOC perhaps not a single byte but derived (e.g. from a coulomb-count or a
  voltage lookup) and simply not published over UDS at all?
- What are the discrete values of Charge Status `B048` across states
  (off / READY-idle / driving / AC charge / DC charge)?
- Does `B04C` (SOH) need an INT16 pair rather than a single byte?

## Confirmed so far

See `01-confirmed-pids.md` — 9 PIDs proven working across BMS (7E5) and VCU (7E3),
plus the ECU request→response map and the known NRC-0x31 dead ends.
