# Phase 3 — Native CAN reader (Waveshare ESP32-S3-RS485-CAN) roadmap

Read the vehicle CAN bus **directly** with the ESP32-S3's built-in TWAI controller
and an onboard CAN transceiver — no ELM327, no Bluetooth. Reuse the decode stack and
dashboard already built in Phase 2. Read-only (Service 22 requests + passive listen);
never write control frames to the vehicle.

```
OBD2 CAN-H/CAN-L ──► [120Ω OFF] TWAI transceiver ──► ESP32-S3 ──WiFi AP──► phone browser
                                                        │
                                                        └─USB-C serial ──► laptop (bring-up)
```

**Why native CAN (vs the ELM327 bridge):**
- **ISO-TP multi-frame** is reliable → unlocks `F194` **BMS software version** (the
  key to locking the SOC transform, H1), `F190` VIN, and per-cell voltages.
- **Passive sniffing** (listen-only) → broadcast frames the OBD request/response
  path never exposes: direct **dash SOC**, **regen level** (H3, currently parked),
  gear, etc.
- **Faster** — no AT text layer, no ELM per-command overhead.
- **No BT** — the ESP32-S3 has no BT Classic, but native CAN doesn't need it, and so
  **WiFi runs alone: the Phase-2 radio-coexistence risk (#1) disappears.**

**Each step below = one iteration** (assumption → build → test → finding → commit),
same rhythm as Phase 2. The detailed plan for a step is written when it is started
(via plan mode); this file is the master reference.

---

## Hardware (confirmed from Waveshare docs)

| Item | Value |
| --- | --- |
| MCU | ESP32-S3, **16 MB** flash, **8 MB** PSRAM, USB-C |
| CAN (TWAI) | **TX = GPIO15, RX = GPIO16**; onboard **120 Ω termination, jumper-selectable** |
| Bus rate | 125 / 250 / 500 k / 1 M — MAXUS OBD = **500 kbps, 11-bit** (ISO 15765-4) |
| Power | screw terminal **7–36 V DC**, or USB-C 5 V |
| BT | BLE only (no BT Classic) — irrelevant for native CAN |

**Source layout:** new ESP-IDF project `firmware/poc5_s3_can/`, target `esp32s3`
(`idf.py set-target esp32s3`). Reuse `firmware/poc4_idf_decode/main/uds.c` + `uds.h`
(parse logic unchanged) and the PID table + formulas; reuse the WiFi AP + HTTP +
tabbed UI from poc3/poc4. Only the **transport** changes: SPP callbacks →
`twai_transmit()` / `twai_receive()`.

## Wiring + safety (Step 0, do before any code)
- OBD2 pin **6 = CAN-H**, pin **14 = CAN-L** → board screw terminal. Pin **16 = +12 V**
  (can power the board, within 7–36 V), pins **4/5 = GND**.
- ⚠️ **Disable the onboard 120 Ω termination jumper.** The vehicle already terminates
  the bus (2 × 120 Ω = 60 Ω); adding a third resistor drops it to ~40 Ω and corrupts
  signalling. We are a diagnostic tap, not a bus end.
- ⚠️ **Start in `TWAI_MODE_LISTEN_ONLY`** — the controller never ACKs or transmits, so
  it physically cannot disturb the vehicle bus. Only switch to `TWAI_MODE_NORMAL` in
  Step 2 when we deliberately send Service-22 requests.
- Need an **OBD2 → screw-terminal harness** (source or solder).

## How each step is verified (read this before asking "how do we know it worked")
- **Steps 1–4 = USB-C serial monitor** (`idf.py monitor`), exactly how poc2 was
  verified before poc3 added WiFi. The firmware `ESP_LOGI`s frames/values; laptop must
  be at the car. The **WiFi dashboard does not exist until Step 5** and is only needed
  for untethered use.
- **Step 5+ = phone over WiFi** (no laptop needed).

---

## Result (2026-08-20) — Steps 1-3 all passed in one session; Step 4 is blocked

`firmware/poc5_s3_can/` on the real vehicle. Full write-up:
`docs/findings/11-native-can-ecu-scan.md`.

**The OBD port is gateway-isolated.** Listen-only saw **zero** frames at all four
bitrates with `bus_err=0`, while the harness measured a healthy **63 Ω** and the
desk self-test had already proven driver/GPIO/transceiver. The same wires answer
a Service-22 request in **~10 ms**. The port answers requests but forwards no
internal bus traffic.

Consequences for this roadmap:
- **Step 1 passes on a revised criterion.** "Frames arrive passively" is
  unachievable on this vehicle *by design*, so the transport is proven by a
  correct request/response round trip instead (Pack Voltage `B042` = 390.25 V).
  The desk self-test remains the check for the driver/pin/transceiver layer.
- **Steps 2 and 3 passed at the same time** — active Service 22 decodes confirmed
  PIDs, and ISO-TP multi-frame delivered `F194` (BMS software version) and `F190`
  (VIN), the capability the ELM327 path never had.
- **Step 4 (passive sniffing) is not possible through this connector** — proven,
  not assumed. **H3 (regen) is therefore rejected via OBD**; it would need a tap
  on an internal bus elsewhere in the vehicle.
- **Bonus:** an ECU/DID sweep of `0x700`-`0x7FF` found **33 ECUs and 1824 readable
  DIDs** (BMS alone exposes 163 where we used ~10), plus 12 per-cell module groups
  that turn out to be single-frame.

---

## Step 1 — Board + TWAI bring-up (listen-only)
- **Goal:** prove the board receives real bus traffic (GPIO15/16 + transceiver + 500 k).
- **Build:** TWAI driver install, 500 kbps, `TWAI_MODE_LISTEN_ONLY`; a task that
  `twai_receive()`s and logs `id / dlc / data` + a periodic `rx / err / bus-state` line.
- **Desk pre-check (no car):** `TWAI_MODE_NO_ACK` self-test / loopback — transmit a
  frame and receive it back with no other node, proving the driver + pin config + build
  before driving to the car.
- **Test (car, engine/READY):** serial shows frames streaming, `rx` climbing, `err=0`,
  `bus=RUNNING`.
- **Success:** real frames flowing over serial with no bus errors.
- **Risk:** wrong TX/RX pins or bit-timing → no frames / error-passive (re-check GPIO
  15/16, 500 k, termination jumper OFF).

## Step 2 — Active UDS poll (normal mode) + port the confirmed PIDs
- **Goal:** request-and-decode the confirmed-9 over native CAN, matching poc4/ELM327.
- **Build:** switch to `TWAI_MODE_NORMAL`; send single-frame Service-22 requests
  (`7E5`/`7E3`, e.g. `02 22 B0 42`), receive the `7ED`/`7EB` response, decode with the
  **reused `uds.c`**. Port the poc4 PID table (drop the DIDs already proven dead on
  MAXUS; keep the sentinels flagged).
- **Test (car READY):** decoded values match poc4 (Pack V, current sign, cell V, temps,
  12 V, charge status) — and arrive faster.
- **Success:** confirmed-9 decode correctly via TWAI; cross-checks against Phase-2 data.
- **Offline check:** the ported request/parse can be traced against the same vectors as
  `src/test_uds.py`.
- **Risk:** first active TX onto the vehicle bus — keep strictly Service-22 read-only.

## Step 3 — ISO-TP multi-frame  ⭐ highest value
- **Goal:** read multi-frame responses the ELM327 port couldn't.
- **Build:** implement ISO-TP: single / first / consecutive frames + send **flow-control**
  (`30 00 00`). Read **`F194` BMS software version**, **`F190` VIN**, and per-cell
  voltages (`B001…`).
- **Test (car READY):** VIN decodes to a sane string; `F194` returns a version → feed
  the SOC transform.
- **Success:** multi-frame works; `F194` obtained.
- **Impact:** `F194` is the value the MAXUS Display-SOC transform is keyed on — this is
  what finally **locks H1** (see `10-poc4-live-crosscheck.md`).

## Step 4b — Regen-level hunt via state diff  ⭐ **PRIMARY PROJECT GOAL**
*Replaces Step 4 for the regen objective, since passive sniffing is unavailable here.*
- **Goal:** find the DID that holds the **regenerative braking level** (High /
  Medium / Low) — **H3**, the project's primary goal.
- **Why this works where sniffing does not:** regen level is a *driver-selected
  setting*, so an ECU must store it, and stored settings are what Service 22 reads.
  The gateway blocks broadcast traffic, not requests.
- **Method:** run the `MODE_SCAN` sweep three times with the vehicle otherwise
  identical (parked, READY, similar SOC), changing only the regen setting:
  **Low → Medium → High**. Then
  `python tools/analyze_scan.py diff low.log high.log` to get candidates, and use
  the **Medium** capture to discard anything that does not step in order.
- **Control for drift:** voltages, temperatures and counters move on their own, so
  a two-way diff returns noise as well as signal. A genuine 3-position setting must
  move **monotonically across all three** captures — that is the discriminator.
- **Success:** one DID (ECU + address) that reliably tracks the regen selector,
  re-verified by setting the level a fourth time and predicting the value.
- **Then:** add it to the poc4/poc5 PID table and the dashboard.
- **Needs the car**, parked and READY, for three scan passes (~12 min each).

## Step 4 — Passive sniffing / broadcast-frame discovery ❌ **not possible via OBD**
*Proven unavailable on this vehicle — kept for the record. See the Result section above.*
- **Goal:** find data that is broadcast on the bus but not on OBD request/response.
- **Build:** `TWAI_MODE_LISTEN_ONLY` full capture (all IDs) → log to serial (or flash);
  a `diff` of two captures at known, different SOC states (reuse the `scanner.py`
  snapshot/diff idea, on-device).
- **Test (car):** capture at two SOC states; find the byte/ID that tracks the **dash**
  SOC; look for regen / gear broadcasts (H3).
- **Success:** identify the dash-SOC broadcast ID (or prove it isn't broadcast), plus
  any regen frame.
- **Risk:** large volume of unknown frames — filter by ID, capture windows.

## Step 5 — WiFi dashboard (no BT, no coexistence)
- **Goal:** untethered live view, no laptop.
- **Build:** reuse poc3/poc4 WiFi SoftAP + `esp_http_server` + the tabbed UI, reading
  the same mutex-guarded snapshot the CAN poller writes. 16 MB flash → no partition
  squeeze; WiFi is the only radio user → simpler and more stable than Phase 2.
- **Test (car):** phone joins the AP, sees live values updating over WiFi while the CAN
  poller runs.
- **Success:** feature-equivalent dashboard served standalone from the S3.

## Step 6 — Consolidate + decide ELM327 retirement
- **Goal:** one native-CAN device; decide whether the ELM327 bridge is retired.
- **Build:** power from OBD 12 V (buck / easy unplug to avoid parasitic drain),
  enclosure, strain relief; keep read-only. Fold in whatever Step 4 discovered.
- **Test (car, driving — safely, ideally a passenger reads):** stable operation; close
  remaining hypotheses (H1 with `F194`, H6 coolant delta, H4 charging states).
- **Success:** stable in-car native-CAN device; ELM327 path kept only as fallback/record.

---

## Notes
- **Reuse-heavy:** `uds.c/uds.h`, the PID table + formulas, and the WiFi+HTTP+tabbed UI
  all carry over from Phase 2 — the genuinely new code is the TWAI transport (Steps 1–2),
  ISO-TP (Step 3), and the sniffer (Step 4).
- **Verification:** USB-C serial for Steps 1–4; phone-over-WiFi from Step 5.
- **Safety invariant:** listen-only by default; the only writes ever sent are Service-22
  read requests + ISO-TP flow-control. Termination jumper stays OFF on a vehicle tap.
- Logging (flash/SD) remains a deferred follow-on, useful for the Step-4 captures and
  safe reading while driving.
