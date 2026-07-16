/*
 * MAXUS 9 EV - ESP32 Bridge  (Phase 2)
 * Step 1: board + toolchain bring-up / board-check.
 *
 * Goal: prove the Arduino build+flash pipeline works AND confirm this board is a
 * classic ESP32 (WROOM-32) that supports Bluetooth Classic (SPP) - the hard
 * requirement for talking to the ELM327 in later steps.
 *
 * The include below is a COMPILE-TIME board gate: BluetoothSerial.h hard-errors on
 * chips without BT Classic (ESP32-S3 / C3 / S2). If this sketch compiles, the board
 * has BT Classic. The runtime chip-model print confirms it a second way.
 *
 * No OBD / Bluetooth / WiFi activity yet - that starts in Step 2.
 * Board: "ESP32 Dev Module". Serial Monitor @ 115200.
 */

#include "BluetoothSerial.h"   // compile-time BT-Classic gate (not used yet)

#ifndef LED_BUILTIN
#define LED_BUILTIN 2           // most ESP32 DevKit boards: onboard LED on GPIO2
#endif

void setup() {
  Serial.begin(115200);
  delay(500);                   // let USB-serial settle

  Serial.println();
  Serial.println("========================================");
  Serial.println(" MAXUS 9 EV - ESP32 Bridge | Step 1 check");
  Serial.println("========================================");
  Serial.printf("Chip model   : %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %d\n", ESP.getChipRevision());
  Serial.printf("CPU cores    : %d\n", ESP.getChipCores());
  Serial.printf("CPU freq     : %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("Flash size   : %u bytes\n", ESP.getFlashChipSize());
  Serial.println("BT Classic   : available (this sketch compiled)");
  Serial.println("----------------------------------------");
  Serial.println("Expect chip model 'ESP32' (WROOM-32). If it reads");
  Serial.println("ESP32-S3 / ESP32-C3, this board is BLE-only and");
  Serial.println("cannot talk to the ELM327 - swap for a WROOM-32.");
  Serial.println("========================================");

  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  static uint32_t beats = 0;

  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);

  Serial.printf("[heartbeat] alive %u s, free heap %u bytes\n",
                ++beats, ESP.getFreeHeap());
}
