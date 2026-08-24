# 14 — Regen write attempt: two clean rejections, one suspect "success"

- **Date:** 2026-08-21
- **Hypothesis under test:** can `B480` (regen level, confirmed readable in
  `13-regen-level-found.md`) also be **written** via UDS Service `2E`?
- **Setup:** `firmware/poc5_s3_can` in MODE_DASH. Added a `/regen/set?level=N`
  endpoint + a dashboard button (Motor tab) that sends
  `04 2E B4 80 <level> AA AA AA` on `7E3` and decodes the `7EB` reply. Board on
  the OBD port, joined to WiFi, tested via the dashboard over several trips.

## What I did
Pressed the Low/Medium/High buttons across different vehicle states: not READY,
READY and stationary, and while driving — both writing the level the car was
already at and writing a different one.

## Raw artifacts
```
vehicle NOT READY   -> 03 7F 2E 7F   (repeatable)
vehicle MOVING       -> 03 7F 2E 88   (repeatable)
one capture, state
not cleanly isolated -> 04 62 B4 80 02
```

## Observations
- `7F 2E 7F`: NRC `0x7F` = **serviceNotSupportedInActiveSession** (ISO 14229-1).
  Not a permission or value problem — Service `2E` simply isn't offered in
  whatever diagnostic session the ECU is in by default. Writes normally require
  at least an **Extended Diagnostic Session** (`10 03`), which this firmware has
  never sent.
- `7F 2E 88`: NRC `0x88` = **vehicleSpeedTooHigh** — a condition-based rejection.
  The ECU evaluated the request and refused it specifically because the vehicle
  was moving, which at least confirms Service `2E` on this DID is *recognized*,
  not just universally unsupported.
- `62 B4 80 02`: **this is a Service 0x22 (Read) positive reply**, not `0x6E`
  (the positive reply for Service `0x2E` Write). It cannot be evidence of a
  successful write.

## A code bug found while investigating, not a vehicle fact
`regen_write()` is the only bus user in `main.c` that never takes `s_bus_mutex`
before calling `twai_transmit`/`twai_receive` — `dash_poll_task` and `scan_task`
both wrap every bus access in it. Because `dash_poll_task` polls this exact DID
(Regen Level, Motor group) every 1-2 s as part of the normal dashboard table, and
both its receive loop and `regen_write()`'s receive loop match replies by CAN id
alone (`0x7EB`, no per-transaction correlation byte exists in these UDS frames),
the two can genuinely consume each other's reply frames.

That makes the `62 B4 80 02` capture almost certainly **not** a write
acknowledgment at all — it is far more likely `dash_poll_task`'s own routine
*read* of Regen Level, picked up by the unmutexed write path at the moment it
happened to be listening. The state around that capture was not cleanly
isolated (unlike the other two, which were reproduced deliberately), which is
consistent with a race rather than a real result.

## Interpretation
Two real, reproducible findings: the write path exists and is recognized by the
ECU (`0x88` proves that), but the default diagnostic session doesn't offer it
(`0x7F`). The third data point is not trustworthy as-is and must be discounted
until the race that could explain it is fixed.

## Verdict
- **H3's read side stays `confirmed`** (unaffected — this note is about writing).
- **H3's write side: `testing`, not concluded.** `0x7F` and `0x88` are solid;
  the "success" is flagged unconfirmed, not treated as evidence either way.
- **Bug identified:** `regen_write()` missing `s_bus_mutex` — fix before any
  further live write test, since without it no result can be trusted.

## Follow-up (2026-08-22) — READY-but-stationary closes the gap: `0x7F` again

The missing state has been tested: **READY, parked, stationary → `0x7F`**, same
as not-READY. Combined with the two states above, all three vehicle states are
now covered:

| State | NRC | Meaning |
| --- | --- | --- |
| Not READY | `0x7F` | session doesn't offer Service `2E` |
| **READY, stationary** | **`0x7F`** | same — session, not vehicle state |
| Moving | `0x88` | speed check, evaluated separately |

This closes the gap without needing to relitigate the mutex fix's relevance
here: `0x7F`/`0x88` are ECU-originated session/condition codes that the race
(described above) cannot fabricate. The race could only ever cause
`regen_write()` to receive an unrelated frame it doesn't recognize — a `0x62`
read-reply meant for `dash_poll_task`'s own routine read of the same DID, which
falls through to the code's "unexpected reply" branch, not a false `0x7F`. So
this data was trustworthy independent of whether the mutex was fixed yet; what
was genuinely missing was only the untested state itself, now filled in.

**Conclusion: the write is gated purely by diagnostic session, not by vehicle
motion.** Parked (READY or not) always hits the session gate first; moving
substitutes a speed gate instead. No state observed so far reaches
`0x33` (securityAccessDenied) — the write path is *closed by session*, not
*locked by security*, as far as tested.

## Follow-up (2026-08-22, second) — Extended Session opens, but `B480` is read-only

The last untested avenue has now been tried. `/regen/set?level=N&ext=1` sends
`10 03` (Extended Diagnostic Session), a `3E 80` TesterPresent, then the `2E`
write, all inside one `s_bus_mutex` hold.

### Raw artifacts
```
Default session:   03 7F 2E 7F     serviceNotSupportedInActiveSession
10 03            -> 50 03          session OPENED
Extended session:  03 7F 2E 31     requestOutOfRange
```
Tried at all three levels (Low / Medium / High), vehicle READY and parked.
Reply arrived in **0.288 s**.

### Observations
- **The session gate can be passed.** `10 03` is accepted (`50 03`) — the ECU
  does not lock Extended Session out, which was one of the possible dead ends.
- **Service `2E` becomes available once in that session.** The NRC moves from
  `0x7F` (service not offered at all) to `0x31` (service ran, request refused).
  That transition is itself the proof the session took effect.
- **`0x31` for every value.** All three valid levels are refused identically, so
  this is not a value-range problem — it is the DID.
- **`0x33` (securityAccessDenied) never appeared** at any point. This is not a
  locked-behind-seed/key situation; the ECU simply does not accept writes here.
- Session expiry behaves as expected: stop polling, wait, and a plain write
  returns to `0x7F`. Note that `dash_poll_task`'s own reads keep the session
  alive while the dashboard is open, since any request resets the S3 timer.

### The "no reply" from the previous follow-up was our own bug
Earlier attempts reported `2E: no reply` twice. That was not ECU behaviour: the
`2E` was being fired immediately after `10 03`, while the ECU was still
switching sessions, and it was dropped. Adding a 100 ms settle delay after the
session change made the reply appear — and it arrives in under 0.3 s, so the
timeout was never the issue. *(The timeout was raised to 6 s in the same change
on a P2\*-timing hypothesis that turned out to be wrong; it is harmless but was
not what fixed this.)* The old code also collapsed "transmit failed" and "no
reply" into one message, which made the two indistinguishable — now separated.

## Verdict
**`B480` is readable but not writable.** It reads as a status mirror of the
regen selector rather than the store the setting actually lives in. The write
path is not blocked by session (passable), nor by security (never invoked), nor
by vehicle state (tested parked and READY) — the ECU refuses the DID itself.

- **H3 read side: `confirmed`** (unchanged, `13-regen-level-found.md`).
- **H3 write side: closed as not possible via this DID.**

## Next
Setting regen level over OBD would require finding whichever DID/ECU actually
*owns* the value rather than mirroring it. That may not exist on the diagnostic
bus at all — this port is gateway-isolated and forwards no internal traffic
(`11-native-can-ecu-scan.md`), so the owning module may simply be unreachable
from here.

What remains fully available is **shadow mode**: regen level is readable live,
so the device can compute and display what level *would* suit the conditions
(following distance, braking) and leave the driver to select it. That needs no
write access and carries none of the risk of automatically changing how the
vehicle decelerates.
