/* SmartFarm ESP32 MAC Reader
 *
 * Diagnostic sketch for the first hardware test only.
 * It does not connect to Wi-Fi, MQTT, the Backend, or OTA.
 * Open Serial Monitor at 115200 baud and record MAC_ADDRESS and CHIP_ID.
 */
#include <WiFi.h>
#include "esp_mac.h"

String chipId() {
  char value[17];
  snprintf(value, sizeof(value), "%012llX", ESP.getEfuseMac());
  return String(value);
}

void printIdentity() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.println("--- SmartFarm ESP32 identity ---");
  Serial.print("MAC_ADDRESS=");
  for (uint8_t i = 0; i < 6; i++) {
    if (i > 0) Serial.print(':');
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
  }
  Serial.println();
  Serial.print("CHIP_ID=");
  Serial.println(chipId());
  Serial.print("CHIP_MODEL=");
  Serial.println(ESP.getChipModel());
  Serial.print("CHIP_REVISION=");
  Serial.println(ESP.getChipRevision());
  Serial.print("FLASH_SIZE_BYTES=");
  Serial.println(ESP.getFlashChipSize());
  Serial.println("--------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("SMARTFARM_MAC_READER_READY");
  printIdentity();
}

void loop() {
  if (Serial.available()) {
    const String command = Serial.readStringUntil('\n');
    if (command == "STATUS") printIdentity();
  }
  delay(100);
}
