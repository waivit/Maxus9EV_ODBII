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
