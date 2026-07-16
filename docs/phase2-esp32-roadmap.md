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

## Step 1 — Board + toolchain bring-up
- **Goal:** confirm hardware + a working build/flash pipeline before any OBD code.
- **Build:** confirm the chip is **ESP32-WROOM-32** (classic BT; NOT S3/C3 which are
  BLE-only). Set up Arduino-ESP32 (or ESP-IDF). Flash a blink + serial "hello".
- **Test (user, bench):** board flashes, LED blinks, serial prints.
- **Success:** toolchain proven; board confirmed BT-Classic-capable.
- **Risk:** wrong chip variant → blocks everything (buy WROOM-32).
- **No car / no ELM327 needed.**

## Step 2 — POC: Bluetooth SPP → ELM327 (1 PID)
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

## Step 3 — POC: WiFi AP + web serve **while** BT polls (coexistence)
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

---

## Notes
- Steps 1–3 are **de-risking** (prove it's possible). Steps 4–6 are **porting +
  deploying** what we already built and documented.
- Verification of hypotheses (H1/H6/H4) only lands at **Step 6**, not during the POC.
- **Logging (SD/flash)** is deferred — a natural follow-on after Step 6, and the
  intended safe path for reading values while driving.
