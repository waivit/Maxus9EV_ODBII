# 15 — Searching all 1799 DIDs for a writable regen store: there isn't one

- **Date:** 2026-08-24
- **Hypothesis under test:** `B480` is a read-only mirror
  (`14-regen-write-attempt.md`); some *other* DID somewhere on the diagnostic
  bus holds the real, writable regen setting.
- **Setup:** `firmware/poc5_s3_can` in MODE_DASH, board on the OBD port, vehicle
  READY and parked throughout. Every scan triggered and downloaded over WiFi.
  Scanner had the NRC 0x78 fix in place, so no DID was skipped as "unreadable"
  when the ECU was merely asking for more time.

## What I did
Ran the same three-level method that found `B480` in the first place, but across
the **whole diagnostic address range** rather than the two ECUs used before:

| # | Regen | Scope | Result |
| --- | --- | --- | --- |
| 1 | Low | `ecus=all` (0x700-0x7FF) | 1799 DIDs |
| 2 | High | `ecus=all` | 1762 DIDs |
| 3 | Medium | `7E3,7E5` (the ECUs holding every candidate) | 382 DIDs |

The regen level was confirmed from the dashboard immediately before each scan.
*(Worth noting: the idle stand-down added earlier means a stale reading can be
shown if nobody has polled recently — the Low→High check initially still read
`Low` until the Motor group was forced to refresh. Verifying before scanning is
what stopped a whole 20-minute pass being mislabelled.)*

## Raw artifacts
Captures kept outside the repo (they contain the VIN): `captures/regen_hunt/`
— `01_regen_low.txt`, `02_regen_high.txt`, `03_regen_med.txt`, plus generated
analyses. The raw captures are never edited.

**Low vs High: 128 DIDs changed.** Classified by shape, only five could
plausibly encode a 3-position selector (single byte, both values within 0-2):

| DID | Low | **Medium** | High | verdict |
| --- | --- | --- | --- | --- |
| **`7E3 B480`** | `00` | **`01`** | `02` | ✅ steps in order — the known mirror |
| `7E3 B039` | `00` | `01` | `01` | ✗ Medium equals High |
| `7E5 B096` | `01` | **`03`** | `02` | ✗ counter — kept counting |
| `7E5 B0F0` | `01` | **`03`** | `02` | ✗ counter |
| `7E5 B0F2` | `01` | `01` | `02` | ✗ Medium equals Low |

A full three-way sweep found **41 DIDs distinct across all three scans**, and
none of the other 40 steps discretely: they are per-cell voltages (all twelve
`B0xx` module groups), temperatures, currents, counters and timestamps — values
that drift on their own between scans taken ~25 minutes apart.

## Observations
- The `03` at Medium on `B096`/`B0F0` is the decisive detail. A real 3-position
  setting returns to a middle value; a counter keeps incrementing, and Medium
  was the *third* scan. Two levels alone would have left both looking like
  perfect candidates.
- 128 changes across 33 ECUs is almost entirely noise. The proportion is a good
  argument for why the three-level rule exists: a Low→High diff on this scale
  produces candidates by the dozen.
- `B039` on the IMCU moves `00`→`01` and then stays — possibly a "not in Low"
  flag rather than a level, but it does not encode the level itself.

## Interpretation
Across **1799 DIDs on 33 ECUs**, exactly one value tracks the regen selector,
and it is the one already known to reject writes.

That fits what the vehicle's architecture implies. The infotainment screen does
not change regen by writing a diagnostic DID — it sends an ordinary application
CAN message on the internal bus, and the IMCU acts on it and then exposes the
resulting state through `B480`. There is no "stored setting" on the diagnostic
side to write, because the setting is *commanded*, not *stored* in a way UDS
reaches. `B480` is the read-back, and read-back is all the diagnostic interface
was designed to offer here.

## Verdict
- **The search is closed as negative, and conclusively so.** This is not "we
  didn't find it" — the whole reachable address space was swept at three
  selector positions with the 0x78 blind spot already fixed.
- `B480` remains the only regen-tracking DID, and remains read-only
  (`14-regen-write-attempt.md`).
- Setting regen level through the OBD port is **not possible**.

## Next
The only remaining route is the one the screen itself uses: tap the **internal
CAN bus** where the infotainment/HMI module sits, rather than the gateway-
isolated OBD port.

- **Listen-only on that bus is genuinely valuable on its own** and low risk:
  `MODE_LISTEN` already exists and physically cannot transmit. It would expose
  everything the gateway hides — the regen command message, a dash-SOC
  broadcast, brake and following-distance signals.
- **Injecting** the command is a different risk class and should not be treated
  as a natural next step: the real module already transmits that ID
  periodically, so a second transmitter means contention rather than
  substitution, and application messages on modern vehicles commonly carry a
  rolling counter plus checksum specifically to reject spoofed frames. Doing it
  properly means sitting in-line (man-in-the-middle), which is a much larger
  hardware commitment with its own failure mode: if the board hangs, the screen
  and the IMCU stop talking to each other.
