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
3. A preview appears → **บันทึกภาพ** saves `TK-YYMM-NNNN.jpg`. The page has two parts:
   - **Summary** (large type): SOH, odometer, raw SOC and the cell-voltage spread.
   - **Technical detail** (small type): highest/lowest cell with index, the
     battery temperature range, the pack voltage, and a short explanation of how
     the spread relates to SOH. It gives no verdict.

   On iPhone you can also long-press the preview to save it. Turn the A/C off
   before generating, so the reading is taken close to rest. The 12-module chart
   is switched off for real reports (`SHOW_MODULES` in `report.js`) until the
   module formula is checked at the car (H7 b/d). Demo always shows it.

**ตัวอย่าง (ไม่ต้องต่อรถ)** renders a sample from canned bytes, with a watermark and
no report number. Use it at the desk to check the layout.

### Read-only safety gate (from 1.0.0)
Every command to the ELM327 passes an allowlist in `main/safety.c`. Only adapter
setup and the read services `01`, `09` and `22` get through, never a VIN request.
Anything else is refused and logged as `BLOCKED`. Replies are scrubbed of
VIN-like data before they are stored or shown. If either boot self-test fails,
the device does not connect to the car. Policy and limits:
[`../docs/inspection-safety.md`](../docs/inspection-safety.md).

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
