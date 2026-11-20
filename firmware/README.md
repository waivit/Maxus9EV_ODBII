# MAXUS 9 EV — ESP32 Bridge (firmware, Phase 2)

Self-built ESP32 device that connects to the ELM327 over Bluetooth Classic, polls +
decodes battery PIDs, and serves a realtime web dashboard over its own WiFi AP so a
phone can watch values while driving. Read-only — never writes to the vehicle.

See the full plan: [`../docs/phase2-esp32-roadmap.md`](../docs/phase2-esp32-roadmap.md).

## Hardware requirement (critical)

Must be a **classic ESP32 (ESP32-WROOM-32)** — it has **Bluetooth Classic (SPP)**,
which the ELM327 uses. **ESP32-S3 / C3 / S2 are BLE-only and will NOT work.**
The Step-1 sketch verifies this for you (it won't even compile on the wrong chip).

## Step 1 — board + toolchain check (`maxus_bridge/`)

Confirms the build/flash pipeline and that the board is BT-Classic-capable. No OBD
code yet.

### Build & flash (Arduino IDE)
1. Open `maxus_bridge/maxus_bridge.ino` in Arduino IDE
   (needs the ESP32 boards package installed).
2. **Tools → Board →** `ESP32 Dev Module`.
3. **Tools → Port →** your board's COM port.
4. Click **Upload**.
5. **Tools → Serial Monitor**, set baud to **115200**.

### Expected serial output
```
========================================
 MAXUS 9 EV - ESP32 Bridge | Step 1 check
========================================
Chip model   : ESP32
Chip revision: ...
CPU cores    : 2
CPU freq     : 240 MHz
Flash size   : 4194304 bytes
BT Classic   : available (this sketch compiled)
...
[heartbeat] alive 1 s, free heap ...
```
The onboard LED blinks at ~1 Hz (LED pin is GPIO2 on most DevKits; if it doesn't
blink, the serial output is the authoritative check).

### Pass criteria
- Compiles + uploads with no error (⇒ BT Classic present).
- `Chip model : ESP32` (not `ESP32-S3` / `-C3`).
- Heartbeat prints (and ideally the LED blinks).

If the chip model is S3/C3 or the compile fails on `BluetoothSerial.h`, the board is
unsuitable — use an ESP32-WROOM-32 before Step 2.

## poc6_report — battery inspection report (ESP-IDF)

poc4 (BT → ELM327 → WiFi dashboard) plus a **report image** for taladkrub (hub
handoff H-0135, hypothesis H7). Plan and scope: Step 7 in
[`../docs/phase2-esp32-roadmap.md`](../docs/phase2-esp32-roadmap.md).

### Build & flash
```
cd firmware/poc6_report
idf.py set-target esp32      # first time only
idf.py build flash monitor
```

### Use
1. Phone joins WiFi `MaxusBridge` (pass `maxus1234`) → open `http://192.168.4.1/`.
2. Tab **รายงาน** → **สร้างรายงาน**. The dashboard pauses for a few seconds while
   the report set is read fresh.
3. A preview appears → **บันทึกภาพ** saves `TK-YYMMDD-HHMM.jpg`. The image is a
   **"รายงานผลการอ่านค่าแบตเตอรี่"**: an A4-proportioned 1414×2000 JPEG
   (~450 KB) with a double frame, the taladkrub shield seal as a faint red
   watermark and as the issuer's stamp, and these sections:
   - **Reference:** report number, car, date and time of the inspection. There is
     no plate or VIN, because the device has neither by design.
   - **Key results:** SOH as a 0–100 numeric bar with no pass/fail bands, then the
     odometer read from the car.
   - **ความหมายของผล:** the SOH sentence. Its wording changes when SOH could not be
     read (`อ่านค่าไม่ได้`) or the model has no SOH (`ไม่มีข้อมูล`).
   - **Supporting data:** raw SOC, cell spread, max/min cell with index,
     temperature range, pack voltage.
   - **Method:** read-only OBD, profile and firmware.
   - **Issuer:** electronic, no person and no signature, then the scope line:
     taladkrub confirms the values were read from the car at that date and
     time, and the report is no guarantee of the battery, the car or the
     odometer. There is no QR, because no public verification page exists yet.

   Wording rule (Design, hub 2026-11-19): the report confirms a reading and
   certifies nothing about the car. The words "รับรอง", "ผ่านการตรวจ" and
   "มาตรฐาน" stay out of the title and away from the seal. The seal marks the
   issuer only.

   The seal shapes are `main/assets/seal.svg`, served at `/seal.svg`. Its Thai
   text is drawn by `report.js`, because an SVG drawn as an image cannot use the
   page's fonts. Fonts are the phone's own (no font files on the device).
   "ตัวอย่างงานออกแบบ" appears in demo only.

   On iPhone you can also long-press the preview to save it. Turn the A/C off
   before generating, so the reading is taken close to rest. The 12-module chart
   is switched off for real reports (`SHOW_MODULES` in `report.js`) until the
   module formula is checked at the car (H7 b/d). Demo always shows it.

**ตัวอย่าง (ไม่ต้องต่อรถ)** renders a sample from canned bytes, with a watermark and
no report number. Use it at the desk to check the layout.

### Report number = time of the inspection
The number is `TK-YYMMDD-HHMM` in Thai time (e.g. `TK-261117-1608`), plus a
device suffix such as `-B` once there is a second device. Nothing is stored on
the board, so swapping boards changes nothing.
- **Time source:** the inspector's phone. The ESP32 has no battery-backed
  clock and no internet. The phone sends UTC epoch seconds with the request,
  and the board adds +7 h (Thailand has no DST), so the phone's timezone
  setting does not matter.
- **No report** if that time is earlier than the firmware's build date or more
  than 5 years after it. The phone shows "ตั้งวันเวลาเป็นอัตโนมัติ". A clock set
  wrong by hand inside that window cannot be detected.
- Two reports in the same minute are refused, so the number is not repeated.
  This uses RAM only. Duplicates across boards or reboots are caught by the
  website (hub H-0140).

### DTC read (H9, testing)
On the **รายงาน** tab, below the report, **อ่าน DTC** reads the trouble codes
(read-only, never clears):
- OBD `03` / `07` / `0A` (stored, pending, permanent), sent functionally on `7DF`,
  so every ECU that answers is listed;
- UDS `19 02 FF` on the BMS `7E5`, the IMCU `7E3` and the CCU `7E4`, with the
  failure type and status flags (confirmed, pending, ...).

The result shows codes only, with no descriptions, and **is not on the report
image**. **บันทึกข้อมูลดิบ (JSON)** saves the raw answers to the phone. The ECU
list is MAXUS-specific until per-model profiles arrive.

### Read-only safety gate (from 1.0.0)
Every command to the ELM327 passes an allowlist in `main/safety.c`. Only adapter
setup and the read services get through: `01`, `09` and `22`, plus the DTC reads
`03`, `07`, `0A` and `19 01/02/0A`. A VIN request never gets through, and
neither does clearing codes.
Anything else is refused and logged as `BLOCKED`. Replies are scrubbed of
VIN-like data before they are stored or shown. If either boot self-test fails,
the device does not connect to the car. Policy and limits:
[`../docs/inspection-safety.md`](../docs/inspection-safety.md).

### Bluetooth: scan and the connect button
WiFi and Bluetooth share one radio on the ESP32, and searching for the ELM327
takes most of it. So when no ELM327 answers, the board scans for about 6 s, then
waits 5, 10, 20 and then 30 s between scans (30 s from then on), which keeps the
phone's WiFi usable at the desk. Plugging in the ELM327 later can therefore take
up to about 40 s to be found, unless you press the button.

The line under the page shows the state next to the BT dot (กำลังค้นหา…,
ค้นหาใหม่ใน N วิ, กำลังเชื่อมต่อ…, เชื่อมต่อแล้ว, ไม่ได้เชื่อมต่อ). The
**เชื่อมต่อ Bluetooth** button starts a scan now and resets the waits. It is
off while connected or while a scan or connect is running, unless that has hung
for 20 s.

A failed connect or a dropped link is **not retried automatically**. The state
becomes ไม่ได้เชื่อมต่อ and you press the button. The button only runs the
Bluetooth search and connect. It sends nothing to the car.

### Status LED (onboard, GPIO2 / "D2")
| LED | Meaning |
| --- | --- |
| slow blink, 1 per second | looking for / connecting to the ELM327 |
| double blip every second | ELM327 connected but the car answers nothing (not READY?) |
| solid on | reading live values |
| fast blink, 5 per second | reading the car for a report |
| 3 quick blinks | report issued |

### Desk check
The boot log must show `uds selftest: N/N PASS` and `safety selftest: N/N PASS`. That is a **regression guard** built
from captured bytes. It is not a verification against the car.
