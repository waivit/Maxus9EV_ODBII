/*
 * MAXUS 9 EV - ESP32 Bridge  (Phase 2)
 * Step 2 POC: connect to the ELM327 over Bluetooth Classic (SPP), run the ELM327
 * init sequence, and read one PID (Pack Voltage). Proves the core BT path.
 *
 * Read-only: only Service 22 reads, never writes to the vehicle.
 * Board: "ESP32 Dev Module" (classic ESP32 / WROOM-32). Serial Monitor @ 115200.
 *
 * ELM327 powers from the OBD port's always-on 12V, so the BT connect + AT init
 * work even with the car OFF. Real PID data (22 B0 42) needs the car READY.
 *
 * API verified against arduino-esp32 core 3.x:
 *   begin(name, true)=master; setPin(pin, len); discover(ms)->BTScanResults*;
 *   device->getName()/getAddress(); connect(BTAddress) / connect(String name).
 */

#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth Classic not enabled - use a classic ESP32 (WROOM-32), not S3/C3.
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// "" = scan + connect-by-address using ESP_SPP_SEC_NONE (unauthenticated, no PIN).
// This tests whether the adapter accepts an ESP32 connection WITHOUT doing legacy
// PIN pairing (which arduino BluetoothSerial does poorly). Power-cycle the adapter
// first so it isn't holding a stale bond from Windows.
#define ELM_NAME ""

static const char *BT_PIN = "1234";      // ELM327 default; some use "0000"

// name substrings that identify an ELM327 adapter (case-insensitive)
static const char *NAME_HINTS[] = {"OBD", "ELM", "VLINK", "VEEPEAK", "NEXPEAK", "VIECAR"};

// ELM327 init sequence - ported verbatim from src/elm327.py:init
static const char *INIT_CMDS[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6"};

BluetoothSerial SerialBT;
bool gConnected = false;

// Read from BT until the ELM327 '>' prompt or timeout.
static String readUntilPrompt(uint32_t timeoutMs) {
  String resp;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialBT.available()) {
      char c = (char)SerialBT.read();
      resp += c;
      if (c == '>') return resp;
    }
    delay(2);
  }
  return resp;  // timed out - return whatever arrived
}

// Send one command, return the cleaned reply (mirrors elm327._send_raw).
static String elmCmd(const String &cmd, uint32_t timeoutMs = 2000) {
  while (SerialBT.available()) SerialBT.read();  // flush stale bytes
  SerialBT.print(cmd);
  SerialBT.write('\r');
  String r = readUntilPrompt(timeoutMs);
  r.replace("\r", " ");
  r.replace(">", "");
  r.trim();
  return r;
}

static bool nameLooksLikeElm(const String &name) {
  String up = name;
  up.toUpperCase();
  for (auto h : NAME_HINTS)
    if (up.indexOf(h) >= 0) return true;
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("\n==============================");
  Serial.println(" POC2: ESP32 BT -> ELM327");
  Serial.println("==============================");

  SerialBT.begin("MaxusBridge", true);   // master / client role
  // setPin intentionally NOT called: we connect unauthenticated (ESP_SPP_SEC_NONE)
  // to avoid arduino BluetoothSerial's broken legacy-PIN pairing. Re-enable only
  // if the adapter refuses unauthenticated links.
  // SerialBT.setPin(BT_PIN, strlen(BT_PIN));
  (void)BT_PIN;

  // --- connect ---
  if (strlen(ELM_NAME) > 0) {
    // Connect by name -> default security -> triggers PIN pairing (1234).
    Serial.printf("Connecting by name '%s' (PIN pairing)...\n", ELM_NAME);
    for (int attempt = 1; attempt <= 3 && !gConnected; attempt++) {
      gConnected = SerialBT.connect(String(ELM_NAME));
      if (!gConnected) {
        Serial.printf("  attempt %d failed, retrying...\n", attempt);
        delay(1500);
      }
    }
  } else {
    Serial.println("Scanning ~10s for Bluetooth Classic devices...");
    BTScanResults *res = SerialBT.discover(10000);
    bool haveTarget = false;
    BTAddress target;
    if (res) {
      for (int i = 0; i < res->getCount(); i++) {
        BTAdvertisedDevice *d = res->getDevice(i);
        String name = String(d->getName().c_str());
        String addr = String(d->getAddress().toString().c_str());
        Serial.printf("  [%d] %-20s %s\n", i, name.c_str(), addr.c_str());
        if (!haveTarget && nameLooksLikeElm(name)) {
          target = d->getAddress();
          haveTarget = true;
          Serial.printf("      ^ looks like an ELM327 - will connect\n");
        }
      }
    }
    if (!haveTarget) {
      Serial.println("No ELM327-like device found. Note its name/MAC above,");
      Serial.println("set ELM_NAME and re-flash.");
      return;
    }
    // Cheap ELM327 SPP adapters use NO link security. The default
    // connect() mask (ENCRYPT|AUTHENTICATE) makes them refuse the connection,
    // so force ESP_SPP_SEC_NONE. Retry a few times (SDP/channel can be slow).
    Serial.printf("Connecting to %s (no-security SPP)...\n", target.toString().c_str());
    for (int attempt = 1; attempt <= 3 && !gConnected; attempt++) {
      gConnected = SerialBT.connect(target, 0, ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER);
      if (!gConnected) {
        Serial.printf("  attempt %d failed, retrying...\n", attempt);
        delay(1000);
      }
    }
  }

  if (!gConnected) {
    Serial.println("BT connect FAILED (try PIN 0000, or confirm the adapter is powered).");
    Serial.println(BT_PIN);
    return;
  }
  Serial.println("BT connected. Running ELM327 init...");

  for (auto cmd : INIT_CMDS) {
    uint32_t t = (cmd[2] == 'Z') ? 3000 : 2000;   // ATZ reset needs longer
    String r = elmCmd(cmd, t);
    Serial.printf("  %-8s -> %s\n", cmd, r.c_str());
  }

  Serial.println("Init done. Setting header 7E5 and polling Pack Voltage (22 B0 42).");
  Serial.println("(Real data needs the car READY; otherwise expect NO DATA/blank.)");
  elmCmd("ATSH 7E5");
}

void loop() {
  static uint32_t last = 0;
  digitalWrite(LED_BUILTIN, (millis() / 500) % 2);   // ~1 Hz blink

  if (gConnected && millis() - last > 3000) {
    last = millis();
    if (!SerialBT.connected()) {
      Serial.println("[warn] BT link dropped.");
      gConnected = false;
      return;
    }
    String r = elmCmd("22 B0 42");
    Serial.printf("[Pack V raw] %s\n", r.c_str());
  }
}
