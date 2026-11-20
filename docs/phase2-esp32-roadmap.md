# Phase 2 — ESP32 BT→WiFi bridge (roadmap)

Self-built ESP32 device that replaces the Windows PC: connects to the ELM327 over
Bluetooth Classic, polls + decodes battery PIDs, and serves a realtime web
dashboard over its own WiFi so a phone can watch values **while driving**.
Read-only (Service 22) only — never writes to the vehicle. In-house C++, our IP.

```
ELM327 ──BT Classic (SPP)──► ESP32 ──WiFi AP (hotspot)──► phone browser
```

**Each step below = one iteration** (own assumption → build → hardware test →
finding → commit). Logging is intentionally **out of scope** for these 6 steps
(planned separately later). The full detailed plan for a step is written when it is
started (via plan mode); this file is the master reference.

Source layout (proposed): firmware under `firmware/maxus_bridge/` (Arduino sketch
or ESP-IDF project); reuse decode logic ported from `src/uds.py` + the PID list and
formulas in `docs/findings/` (esp. `07-mg-ev-reference.md`).

---

## Step 1 — Board + toolchain bring-up  ✅ DONE (2026-07-16)
- **Result:** PASS. Chip `ESP32-D0WD-V3` rev 301 (classic dual-core ESP32, same
  silicon as WROOM-32) → BT Classic confirmed (sketch compiled + chip is not S3/C3);
  240 MHz, 4 MB flash, ~300 KB free heap; LED blink + serial heartbeat OK. Board is
  suitable for Step 2. Sketch: `firmware/maxus_bridge/`.
- **Goal:** confirm hardware + a working build/flash pipeline before any OBD code.
- **Build:** confirm the chip is **ESP32-WROOM-32** (classic BT; NOT S3/C3 which are
  BLE-only). Set up Arduino-ESP32 (or ESP-IDF). Flash a blink + serial "hello".
- **Test (user, bench):** board flashes, LED blinks, serial prints.
- **Success:** toolchain proven; board confirmed BT-Classic-capable.
- **Risk:** wrong chip variant → blocks everything (buy WROOM-32).
- **No car / no ELM327 needed.**

## Step 2 — POC: Bluetooth SPP → ELM327 (1 PID)  ✅ DONE (2026-07-16, ESP-IDF)
- **Goal:** prove the ESP32 can talk to the ELM327 over BT Classic.
- **Build:** `BluetoothSerial` as SPP master; discover/connect to the ELM327
  (by name or MAC); port the AT init sequence from `src/elm327.py`
  (`ATZ ATE0 ATL0 ATS0 ATH1 ATAT2 ATST96 ATSP6`); send one request
  (`ATSH 7E5` + `22 B0 42`) and print the raw reply to serial.
- **Test (user):** ELM327 alone answers `ATZ → ELM327 v1.5` (adapter replies to AT
  without the car); then with the **car READY**, `22 B0 42` returns a real frame.
- **Success:** raw `7ED05 62 B042 …` bytes seen over BT on the ESP32 serial.
- **Risk:** BT pairing/PIN, SPP-client quirks, finding the ELM327 address.
- **Needs ELM327 (AT part) + car READY (real PID part).**

### Finding (2026-07-16) — Arduino path ruled out, pivoting to ESP-IDF
Two Arduino (`arduino-esp32` `BluetoothSerial`) attempts, both on real hardware
(adapter name `OBDII`, MAC `aa:bb:cc:11:22:33`):
1. **Default security** — `connect(address)` and `connect(name)` both failed.
2. **Unauthenticated** (`ESP_SPP_SEC_NONE`, no `setPin`) — scan found the adapter
   fine, but `connect()` **FAILED all 3 retries**.

The user separately confirmed that pairing this adapter from Windows **requires
PIN 1234** (an incorrect PIN fails pairing outright) — i.e. this is a **legacy
PIN-pairing** device, not SSP/Just-Works. Combined with (2) failing, the adapter
only accepts an **authenticated, legacy-PIN-bonded** link. Arduino's
`BluetoothSerial` cannot reliably drive legacy PIN pairing (SSP is baked in;
`setPin()` doesn't force legacy mode) — this is a known upstream limitation, now
confirmed by hardware evidence, not just literature.

**Decision:** move Step 2 to **ESP-IDF**, where `menuconfig` can disable SSP and
the app answers the legacy PIN request directly (`esp_bt_gap_set_pin` /
`esp_bt_gap_pin_reply`), matching what Windows does. New POC:
`firmware/poc2_idf_spp/`. The Arduino sketch `firmware/poc2_bt_elm327/` is kept
as-is — a record of the ruled-out path, not part of the shipped bridge.

### Result (2026-07-16) — ESP-IDF POC PASSED end to end
`firmware/poc2_idf_spp/` (ESP-IDF v5.3.3) connects and reads a real PID. Full
serial trace verified live: legacy PIN auth → RFCOMM open → all 8 AT init cmds →
one PID read:
```
Legacy PIN auth SUCCESS with OBDII
BT connected (handle=129)
--> ATZ      <-- ELM327 v1.5
--> ATE0..ATSP6, ATSH 7E5   <-- OK (each)
--> 22 B0 42 <-- 7ED0562B0420644   → 0x0644 * 0.25 = 401.0 V (Pack Voltage B042)
```
Four fixes were needed vs the naive port, each confirmed by hardware:
1. **Controller mode `BR/EDR-only`** (`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY`) — the
   default BTDM mode made `esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)` abort
   with `ESP_ERR_INVALID_ARG`.
2. **Match by MAC, not name** — this adapter's inquiry result carries no BDNAME
   (`found device: (no name)`); name matching never fires. Known MAC hard-coded.
3. **L2CAP ERTM disabled** via `esp_spp_enhanced_init(.enable_l2cap_ertm=false)` —
   the adapter (BT 2.x, Basic Mode only) rejected ERTM config with an immediate
   `Incompatible CFG` L2CAP failure; this was the real connect blocker (SSP-off +
   `SEC_AUTHENTICATE|ENCRYPT` alone did not fix it).
4. **Sanitize AT replies before logging** — the ELM327 emits non-UTF-8 noise bytes
   while it resets after ATZ (hexdump: `...5A 0D **FC** 0D...`); printed raw they
   crashed esp-idf-monitor on the Windows cp1252 console (looked like a hang). Log
   now maps non-printable → space, drops `>`, trims (mirrors `elm327.py`), plus a
   hex dump per reply. Monitor also run with `chcp 65001` + `PYTHONIOENCODING=utf-8`.

**Reusable facts for later steps:** adapter BT name `OBDII`, MAC
`aa:bb:cc:11:22:33`, legacy PIN `1234`, SPP SDP channel `1`. Config lives in
`firmware/poc2_idf_spp/sdkconfig.defaults` (SSP off, Classic BT, SPP, BR/EDR-only).

## Step 3 — POC: WiFi AP + web serve **while** BT polls (coexistence)  ✅ DONE (2026-07-16)
- **Goal:** prove BT Classic + WiFi run together on the single 2.4 GHz radio.
- **Build:** bring up SoftAP (SSID/pass); `WebServer` serving a tiny page + a
  `/data` endpoint returning the one PID from Step 2; keep the BT poll loop running
  in parallel (FreeRTOS task or interleaved loop, shared value under a mutex).
- **Test (user):** phone joins the ESP32 AP, opens the page, sees the value refresh
  live while BT keeps polling — no dropouts/hangs for a few minutes.
- **Success:** stable simultaneous BT poll + WiFi serve (the key feasibility gate).
- **Risk:** **coexistence instability** — the #1 project risk. Mitigations: SoftAP
  (not STA), low poll rate, watch for radio starvation.
- **Needs ELM327; car READY only to show a real value (coexistence itself provable
  with AT-only traffic).**

### Result (2026-07-16) — PASSED, the coexistence gate is cleared
`firmware/poc3_idf_coexist/` (evolves poc2 + WiFi SoftAP + HTTP). Verified live:
WiFi AP (`MaxusBridge` / `192.168.4.1`) + HTTP up in ~1 s, BT connected, and the
BT poll ran **#1→#107 continuously (all `valid=1`) for ~2.8 min** with a phone
watching the value live over WiFi — **no reboot, no BT/WiFi dropout**, free heap
settled and plateaued (~101.7 KB, no leak). BT+WiFi coexistence on one radio is
proven — the whole bridge concept is feasible.

**Architecture (reused by Step 4/5):** SPP RX callback → FreeRTOS stream buffer →
`poll_task` (synchronous write→read-until-`>`-or-timeout, mirrors `webdash.py`
`poller_loop` / `elm327.py:_send_raw`) → mutex-guarded snapshot → HTTP `/` + `/data`.
Needs a **custom `partitions.csv` + 4 MB flash** (WiFi+HTTP push the app to 1.4 MB,
over the default 1 MB partition).

**Dashboard shows 4 values** (2×2 grid): Pack Voltage + Batt Temp (confirmed) and
**SOC (B046) + Coolant (B05C) — TESTING candidates, tagged "test", NOT verified**.
The on-device values (incl. batt temp) are **not live-verified against the car**
yet; value correctness is deferred to the Step-6 field checklist. Step 3's pass is
about **coexistence + the dashboard pipeline**, not the numbers.

## Step 4 — Port the full poller + UDS decode
- **Goal:** read and decode all known PIDs, not just one.
- **Build:** port `src/uds.py` `parse()` (11-bit single-frame) to C++; port the
  confirmed PID list + formulas (`01-confirmed-pids.md`) and the MG-referenced leads
  (SOC B046, SOH B061, coolant B05C, status B048 — `07-mg-ev-reference.md`), with
  the corrected current sign (H5). Poll them into a snapshot struct.
- **Test (user, car READY):** values match what `webdash.py` shows on the PC for the
  same car state (cross-check against the known-good Python stack).
- **Success:** all confirmed PIDs decode correctly on the ESP32.
- **Offline check possible:** unit-test the ported `parse()` on-host against the same
  captured vectors as `src/test_uds.py` before flashing.
- **Needs car READY for the live cross-check.**

## Step 5 — Port the web dashboard
- **Goal:** the full webdash UI, served from the ESP32.
- **Build:** move `webdash.py`'s HTML/JS into PROGMEM; `/data` returns the full
  snapshot JSON (raw hex + ASCII + value + status per PID, same shape); interval
  control; state pill neutral (H5 fix already reflected).
- **Test (user):** phone browser shows the same dashboard as the PC webdash, auto-
  refreshing, over the ESP32 AP.
- **Success:** feature-parity dashboard on the phone, standalone on the ESP32.
- **Needs ELM327 + car for a live view.**

## Step 6 — In-car hardening + field verification
- **Goal:** a permanently-mountable device, and use it to close open hypotheses.
- **Build:** 12V→5V buck for power; power switch / easy unplug (avoid 12V parasitic
  drain when parked); enclosure; strain relief. Keep it read-only.
- **Test (user, driving — safely, ideally a passenger reads):** run the at-car
  checklist from `07-mg-ev-reference.md` live:
  - **H1 SOC (B046):** tracks dash across high/low SOC → fit lo/hi.
  - **H6 coolant (B05C):** rises under sustained drive / DC charge.
  - **H4 status (B048):** shows 06 (AC) / 07 (DC) while charging.
- **Success:** stable in-car operation **and** H1/H6/H4 verified (one build, several
  birds). *(Safe realtime reading while driving is why logging will be added later.)*
- **Needs full install + driving.**

## Step 7 — Inspection report (JPG) for taladkrub  🔧 (hub handoff H-0135, hypothesis H7)
- **Goal:** one tap on the phone produces a **battery inspection report image** a
  buyer can read. The image is uploaded to taladkrub by hand; the device itself never
  touches the internet.
- **Build:** `firmware/poc6_report/` is a copy of poc4 plus:
  - **ISO-TP reassembly** in `uds.c` (first + consecutive frames, sequence check,
    NRC `0x78` skipped). The ELM327 already sends flow control. Only the parser was
    missing (note 11, correction 2026-11-17).
  - **`/report`**: the dashboard poller pauses (shared ELM mutex). The device reads
    the report set fresh, retries a failed DID once, bumps an NVS counter, and
    returns JSON. With `?demo=1` it decodes canned raw bytes instead, does not count,
    and the image carries a watermark.
  - **`/report.js`**: the phone's browser draws the report on a Canvas (Thai text from
    the phone's own fonts, logo drawn from its SVG paths) and saves it as JPEG
    1080 px wide, ≤ 2000 px tall.
  - **Module tab** on the dashboard: the 12 groups `B001`…`B0CB`, tagged `test`.
    They are not on the image (formula unverified).
- **Report content (from Design, H-0135):** logo + title · report number
  `TK-YYMM-NNNN` · date/time · profile `maxus9` + firmware · SOH ("value reported
  by the car's battery system, not yet compared with a service centre") · odometer
  ("supporting information") · cell max/min + index + spread in mV (raw) · temp
  hi/lo + sensor index · raw SOC. **Not on it:** VIN, good/bad colouring, any
  conclusion about battery replacement, the phrase "anti-odometer-rollback".
- **Revised the same day (user):** page 2 was folded back into **one page**: a
  large-type summary (SOH, odometer, raw SOC, cell spread) above a small-type
  technical block (highest/lowest cell + index, temperature range, pack V, short
  explanation). Pack current, sensor indices, BMS state and the estimated cell
  count were dropped. The cell count has no spec, so the user chose to show
  nothing rather than an estimate. The text below records the intermediate
  two-page version.
- **Page 2, cell detail (added 2026-11-17 at the user's request):** a second JPG.
  It shows the measurement conditions (pack V `B042`, current `B043`, raw SOC, BMS
  state `B048`), the series cell count estimated from pack V ÷ mean cell V, the
  highest/lowest cell with index and the spread, and the temperature spread. It ends
  with a fixed explanation: the spread is balance, not health, and how it relates to
  SOH. There is **no verdict**. The proposed assessment bands
  (`findings/16-cell-spread-references.md`) wait for Design. The 12-module chart is
  shown only in demo until H7 (b)+(d) pass.
- **Desk check (no car):** boot log prints the parser self-test (`uds selftest: N/N
  PASS`) and `?demo=1` renders a watermarked sample. Regression guard only.
- **Test (user, car READY):** generate a report and compare each value with the
  dashboard tabs. Check that the Module tab fills in.
- **Success:** H7 (a) + (b) judged by the user at the car. One real report (VIN-free)
  is dropped into the hub for the taladkrub web side.

## Step 8 — Inspection device for any model (hub handoff H-0138)
taladkrub inspects any EV/PHEV/HEV for free. Models without a profile are
explored on site. The user decided on **ELM327 only** (no native-CAN board), and
every mode is **read-only**.

| Stage | Work | Status |
| --- | --- | --- |
| 1 | Safety gate: command allowlist, VIN scrub, 29-bit CAN parsing, version `1.0.0`, JPEG ≤ 2 MB | desk ✅ (self-tests 14/14 + 66/66) · car pending (H8) |
| 1b | DTC reading (`03 07 0A` on `7DF`, `19 02 FF` on `7E5/7E3/7E4`), opened at the user's request. Shown on the phone with a raw JSON download, not on the report image | desk ✅ (self-tests 24/24 + 80/80, synthetic replies) · car pending (H9) |
| 1c | Report number `TK-YYMMDD-HHMM` from the phone's clock (UTC + 7 h). No NVS counter, so it does not depend on the board. Refused if the time is implausible or the same minute repeats | desk ✅ (board self-test report 10/10) · not committed (user) |
| 1d | Report redesigned as "รายงานผลการอ่านค่าแบตเตอรี่" (A4 proportion, seal watermark and issuer stamp, "ความหมายของผล" box, no plate/VIN, no QR, no signature). Renamed from "หนังสือรับรอง…" per Design (2026-11-19): it confirms a reading, it certifies nothing, so no "รับรอง" in the title or next to the seal | desk: rendered for full data, SOH unread, SOH absent, partial data and demo · 1414×2000, ~450 KB · phone render checked by the user · not committed (user) |
| 2 | Per-model profiles as JSON files on the device, reduced report (no SOH: "ยังอ่านค่า SOH ของรุ่นนี้ไม่ได้"). The not-at-rest current warning is dropped (user, 2026-11-19) | planned |
| 3 | Discovery mode: find ECUs, identification DIDs (no VIN), Mode 01, bounded DID sweep, raw log tied to the report number, kept on the board until the phone has downloaded it | planned |
| 4 | Listen-only (`ATMA` + `ATCSM1`), time-boxed | planned |
| 5 | Next profiles from public data (MG ZS EV / MG4 first), unverified until a real car | planned |

Policy: [`inspection-safety.md`](inspection-safety.md).

---

## Notes
- Steps 1–3 are **de-risking** (prove it's possible). Steps 4–6 are **porting +
  deploying** what we already built and documented.
- Verification of hypotheses (H1/H6/H4) only lands at **Step 6**, not during the POC.
- **Logging (SD/flash)** is deferred — a natural follow-on after Step 6, and the
  intended safe path for reading values while driving.
