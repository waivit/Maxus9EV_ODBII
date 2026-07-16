# 03 — uds.parse decodes 11-bit CAN responses as "no data"

- **Date:** 2026-07-16
- **Hypothesis under test:** parser bug (blocks H1/H2/H4 — nothing decodes)
- **Setup:** ELM327 v1.5 on COM7 (Bluetooth), vehicle READY, CAN 11/500 (`ATSP6`),
  headers on (`ATH1`), spaces off (`ATS0`), Service 22.

## What I did
Connected live and sent Service 22 requests to the BMS (7E5) and VCU (7E3), then
compared the raw ELM responses against what `uds.parse()` returned.

## Raw artifacts
Every PID gave a valid positive response, yet `parse()` returned `no data`.
Captured single-frame responses (id · PCI · service+DID · data):

```
7ED0562B041065E   -> 7ED 05 62 B041 06 5E     Bus Voltage
7ED0562B042065F   -> 7ED 05 62 B042 06 5F     Pack Voltage
7ED0562B0439C90   -> 7ED 05 62 B043 9C 90     Pack Current
7ED0562B0568F06   -> 7ED 05 62 B056 8F 06     Batt Temp High
7ED0662B0580EC20B -> 7ED 06 62 B058 0E C2 0B  Cell Voltage Max
7EB046201120E     -> 7EB 04 62 0112 0E        12V Battery (VCU)
7ED0462B04C03     -> 7ED 04 62 B04C 03        (previously mislabelled SOH)
7ED037F2231       -> 7ED 03 7F 22 31          NRC 0x31 (requestOutOfRange)
```

## Observations
- The CAN ID is **3 hex chars** (`7ED`, `7EB`) — 11-bit addressing.
- Layout is `III` + `0N` (ISO-TP single-frame PCI, `N` = payload byte count) +
  payload. e.g. `7ED 05 62B041065E` → 5 payload bytes `62 B0 41 06 5E`.
- `uds.parse()` assumes a **6-char header** (`header = s[0:6]`, uds.py:47). It
  slices `7ED056...`, so the service byte is read as `B0` instead of `62`, the
  `svc == "62"` positive branch never matches, and the value is dropped.

## Interpretation (assumption to encode)
MAXUS runs **11-bit CAN only** (`ATSP6`). Correct single-frame parse:
`ident = s[0:3]`, `N = int(s[4],16)`, `payload = s[5 : 5+2N]`, then the existing
`62`/`7F` logic. All 9 confirmed PIDs are single-frame (≤6 payload bytes); a
multi-frame path (`PCI '1'`/`'2'`) is added best-effort but is unverified.

## Verdict
- **confirmed (offline).** `parse()` rewritten for 11-bit CAN (`ident = s[0:3]`,
  single frame `payload = s[5:5+2N]`); `src/test_uds.py` decodes all captured
  vectors correctly and the negative case yields `nrc_code == 0x31`. Decoded
  values are physically sane (Bus 407.5 V, Cell max 3.778 V). Live re-check at the
  vehicle still pending (separate session).

## Next
Re-run `webdash.py` on COM7 at the car to confirm the dashboard shows real values
instead of "no data" (deferred). Then chase SOC (H1, candidate B05C) and the true
SOH PID (H2 — B04C ruled out, see below).

## Result (2026-07-16, offline)
`python test_uds.py` → **ALL PASS** (8 positive DIDs incl. 3-byte cell data and
the 7EB/0112 VCU frame; 1 NRC 0x31; 2 physical-sanity checks). The fix is shared,
so reader / scanner / diag / webdash all benefit without changes.
