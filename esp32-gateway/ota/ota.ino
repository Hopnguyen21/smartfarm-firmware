/*
 * SmartFarm ESP32-C3 Gateway v2
 *
 * Firmware v2 keeps the original C3 firmware untouched.  Gateway identity and
 * Node routing are loaded at runtime through Serial and persisted in NVS.
 * No Farm, Zone, Gateway or Node name is compiled into this firmware.
 *
 * Send one JSON object through Serial Monitor (115200), for example:
 * {"gatewayId":"...","gatewaySerial":"...","farmId":"...","zoneId":"...",
 *  "mqtt":{"host":"192.168.1.160","port":1883,"clientId":"...","fingerprint":"..."},
 *  "nodes":[{"deviceId":"...","hardwareAddress":"...","loraAddress":1,"deviceType":"SensorNode"}]}
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <vector>
#include <time.h>
// E32 wiring kept compatible with the original C3 firmware.
static const uint8_t LORA_NETWORK_ID = 0x11;
static const uint8_t LORA_CHANNEL = 0x1A;
static const int PIN_E32_RX = 6;
static const int PIN_E32_TX = 7;
// Current Gateway wiring: GPIO4=M0, GPIO5=M1, GPIO6=RX, GPIO7=TX.
static const int PIN_E32_M0 = 4;
static const int PIN_E32_M1 = 5;
// Allow enough time for the E32 air-rate and the UNO node sensor loop.
static const unsigned long NODE_REPLY_TIMEOUT_MS = 5000;
static const unsigned long SENSOR_POLL_INTERVAL_MS = 8000;
static const char* FIRMWARE_VERSION = "1.0.10";
static const char* BACKEND_URL = "http://192.168.1.159:5144";
// Temporary recovery mode: use the known-good deployment config without
// requiring CONFIG=... to be pasted through Serial Monitor.
static const bool USE_COMPILED_CONFIG = false;
static const char* COMPILED_CONFIG = R"json({"gatewayId":"5aae6a76-8f81-4786-9c61-9ca8e33948dc","gatewaySerial":"GW-DEMO-001","farmId":"51ce6b77-fd85-43d5-911d-6f3469789bf7","zoneId":"0b6eb24a-38a5-47c2-ab4c-372b3489b724","mqtt":{"host":"192.168.1.159","port":1883,"clientId":"smartfarm-gateway-5aae6a768f8147869c619ca8e33948dc","fingerprint":"8EC678A33E9B10527792357E2B380B96D950631F607E883D41EA5E5B9C3BC4E6"},"nodes":[{"deviceId":"d7aaa79c-746a-4148-8339-c344971a66ec","hardwareAddress":"NODE-ZONE1-02","zoneId":"0b6eb24a-38a5-47c2-ab4c-372b3489b724","loraAddress":2,"deviceType":"SensorNode"},{"deviceId":"fdfcff82-b065-4f10-8a23-84a467f07c15","hardwareAddress":"NODE-ZONE1-01","zoneId":"0b6eb24a-38a5-47c2-ab4c-372b3489b724","loraAddress":3,"deviceType":"SensorNode"}]})json";
struct NodeRoute { String deviceId; String hardwareAddress; uint8_t loraAddress; String deviceType; };
struct GatewayConfig {
  String gatewayId, gatewaySerial, farmId, zoneId, mqttHost, mqttClientId, fingerprint;
  uint16_t mqttPort = 1883;
  int configVersion = 1;
  int telemetryIntervalSec = 60;
  int samplesPerInterval = 1;
  int pollingIntervalSeconds = 60;
  int heartbeatIntervalSeconds = 15;
  int responseTimeoutMs = 3000;
  int retryCount = 2;
  std::vector<NodeRoute> nodes;
};
Preferences prefs;
GatewayConfig config;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HardwareSerial e32Serial(1);
String serialLine;
unsigned long lastPublish = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastOtaPoll = 0;
uint8_t currentNodeIndex = 0;
bool enrollmentHandshakeVerified = false;

bool applyOtaUpdate(const String& firmwareUrl, const String& expectedSha256) {
  if (firmwareUrl.isEmpty() || expectedSha256.length() != 64) {
    Serial.println("OTA_REJECTED:INVALID_METADATA");
    return false;
  }
  HTTPClient http;
  if (!http.begin(firmwareUrl)) { Serial.println("OTA_DOWNLOAD_FAILED:BEGIN"); return false; }
  const int status = http.GET();
  if (status != HTTP_CODE_OK) { Serial.printf("OTA_DOWNLOAD_FAILED status=%d\n", status); http.end(); return false; }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  uint8_t buffer[1024]; size_t total = 0;
  WiFiClient* stream = http.getStreamPtr();
  while (http.connected() && (stream->available() || total == 0)) {
    const int available = stream->available();
    if (available <= 0) { delay(1); continue; }
    const size_t readCount = stream->readBytes(buffer, min((int)sizeof(buffer), available));
    if (!readCount) break;
    mbedtls_sha256_update(&sha, buffer, readCount); total += readCount;
  }
  uint8_t digest[32]; mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha); http.end();
  String actual; actual.reserve(64);
  for (uint8_t b : digest) { if (b < 16) actual += '0'; actual += String(b, HEX); }
  if (!total || !actual.equalsIgnoreCase(expectedSha256)) {
    Serial.println("OTA_SHA256_MISMATCH");
    return false;
  }
  if (!http.begin(firmwareUrl)) { Serial.println("OTA_DOWNLOAD_FAILED:BEGIN_WRITE"); return false; }
  const int writeStatus = http.GET();
  if (writeStatus != HTTP_CODE_OK || !Update.begin(UPDATE_SIZE_UNKNOWN)) { Serial.printf("OTA_WRITE_FAILED status=%d\n", writeStatus); http.end(); return false; }
  size_t written = Update.writeStream(*http.getStreamPtr());
  const bool ok = written > 0 && Update.end(true) && Update.isFinished();
  http.end();
  if (!ok) { Serial.println("OTA_WRITE_FAILED:VERIFY"); return false; }
  Serial.println("OTA_APPLIED"); delay(500); ESP.restart(); return true;
}

void pollRuntimeUpdate() {
  if (WiFi.status() != WL_CONNECTED || millis() - lastOtaPoll < 10000) return;
  lastOtaPoll = millis();
  HTTPClient http;
  String url = String(BACKEND_URL) + "/api/v1/gateway-runtime/update?macAddress=" + WiFi.macAddress() + "&currentVersion=" + FIRMWARE_VERSION;
  if (!http.begin(url)) return;
  const int status = http.GET();
  if (status == HTTP_CODE_OK) {
    StaticJsonDocument<768> doc;
    if (!deserializeJson(doc, http.getString())) {
      const String firmwareUrl = doc["firmwareUrl"] | "";
      const String sha = doc["artifactSha256"] | "";
      Serial.println("OTA_JOB_RECEIVED");
      http.end(); applyOtaUpdate(firmwareUrl, sha); return;
    }
  }
  http.end();
}
uint16_t calculateCRC16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; bit++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}
void sendE32Frame(uint8_t address, const uint8_t* payload, size_t length) {
  // Match the original working Gateway: allow E32 to leave config/previous
  // TX state before accepting the fixed-transmission header.
  delay(25);
  // Fixed transmission header: [ADDH, ADDL, CHANNEL].
  Serial.print("LORA_TX_BEGIN addh=0x");
  if (LORA_NETWORK_ID < 16) Serial.print('0');
  Serial.print(LORA_NETWORK_ID, HEX);
  Serial.print(" addl=0x");
  if (address < 16) Serial.print('0');
  Serial.print(address, HEX);
  Serial.print(" channel=0x");
  if (LORA_CHANNEL < 16) Serial.print('0');
  Serial.print(LORA_CHANNEL, HEX);
  Serial.print(" bytes=");
  Serial.print(3 + length);
  Serial.print(" data=");
  Serial.print(LORA_NETWORK_ID, HEX); Serial.print(' ');
  Serial.print(address, HEX); Serial.print(' ');
  Serial.print(LORA_CHANNEL, HEX);
  for (size_t i = 0; i < length; i++) {
    Serial.print(' ');
    if (payload[i] < 16) Serial.print('0');
    Serial.print(payload[i], HEX);
  }
  Serial.println();
  e32Serial.write(LORA_NETWORK_ID);
  e32Serial.write(address);
  e32Serial.write(LORA_CHANNEL);
  e32Serial.write(payload, length);
  e32Serial.flush();
  Serial.println("LORA_TX_FLUSHED");
}
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT_COMMAND topic=");
  Serial.print(topic);
  Serial.print(" payload=");
  for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
  Serial.println();
  String raw;
  raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) raw += (char)payload[i];
  StaticJsonDocument<8192> command;
  if (deserializeJson(command, raw) || String((const char*)(command["type"] | "")) != "CONFIG") return;
  const int requestedVersion = command["configVersion"] | 0;
  if (requestedVersion <= config.configVersion) {
    Serial.printf("CONFIG_IGNORED version=%d current=%d\n", requestedVersion, config.configVersion);
    return;
  }
  if (!saveConfig(raw)) { Serial.println("CONFIG_REJECTED"); return; }
  StaticJsonDocument<256> ack;
  ack["type"] = "CONFIG_ACK";
  ack["gatewayId"] = config.gatewayId;
  ack["configVersion"] = config.configVersion;
  String ackPayload; serializeJson(ack, ackPayload);
  String ackTopic = "smartfarm/gateways/" + config.gatewayId + "/config-ack";
  mqtt.publish(ackTopic.c_str(), ackPayload.c_str());
  Serial.printf("CONFIG_APPLIED version=%d\n", config.configVersion);
}
void setE32ConfigMode(bool enabled) {
  digitalWrite(PIN_E32_M0, enabled ? HIGH : LOW);
  digitalWrite(PIN_E32_M1, enabled ? HIGH : LOW);
  delay(50);
}
bool checkAndConfigureE32() {
  Serial.println("LORA_CHECK_START");
  setE32ConfigMode(true);
  while (e32Serial.available()) e32Serial.read();
  const uint8_t readCommand[3] = {0xC1, 0xC1, 0xC1};
  e32Serial.write(readCommand, sizeof(readCommand));
  e32Serial.flush();
  uint8_t response[6]{};
  uint8_t count = 0;
  const unsigned long started = millis();
  while (millis() - started < 700 && count < sizeof(response)) {
    if (e32Serial.available()) response[count++] = (uint8_t)e32Serial.read();
    delay(2);
  }
  if (count < 6 || (response[0] != 0xC0 && response[0] != 0xC2)) {
    setE32ConfigMode(false);
    Serial.println("LORA_CHECK_FAILED reason=NO_E32_RESPONSE");
    return false;
  }
  Serial.printf("LORA_CONFIG_RAW C0 %02X %02X %02X %02X %02X\n",
                response[1], response[2], response[3], response[4], response[5]);
  Serial.printf("LORA_CONFIG_READ ADDH=0x%02X ADDL=0x%02X SPED=0x%02X CHANNEL=0x%02X OPTION=0x%02X\n",
                response[1], response[2], response[3], response[4], response[5]);
  // Original firmware uses the Network ID as E32 ADDH.
  // Configuration is provisioned once externally. Do not rewrite E32
  // parameters during boot; this keeps the known-good radio state intact.
  // SPED: 0x1A = 9600 bps UART + 2.4 kbps Air rate
  if (response[1] != LORA_NETWORK_ID || response[2] != 0x00 || response[3] != 0x1A ||
      response[4] != LORA_CHANNEL || response[5] != 0xC4) {
    Serial.println("LORA_CONFIG_WARNING: Config mismatch, writing SPED=0x1A (9600bps)...");
    uint8_t writeCmd[6] = {0xC0, LORA_NETWORK_ID, 0x00, 0x1A, LORA_CHANNEL, 0xC4};
    e32Serial.write(writeCmd, sizeof(writeCmd));
    e32Serial.flush();
    delay(100);
    // Read response
    count = 0;
    while (e32Serial.available() && count < sizeof(response)) response[count++] = (uint8_t)e32Serial.read();
    Serial.printf("LORA_CONFIG_UPDATED C0 %02X %02X %02X %02X %02X\n",
                  response[1], response[2], response[3], response[4], response[5]);
  }
  setE32ConfigMode(false);
  delay(100);
  Serial.println("LORA_CHECK_OK");
  return true;
}
String utcNow() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 1000)) return "";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return String(buffer);
}
bool validHex64(const String& value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); i++) if (!isxdigit(value[i])) return false;
  return true;
}
bool loadConfig() {
  String raw = USE_COMPILED_CONFIG ? String(COMPILED_CONFIG) : prefs.getString("config", "");
  if (raw.isEmpty()) return false;
  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, raw)) return false;
  config.gatewayId = doc["gatewayId"] | "";
  config.gatewaySerial = doc["gatewaySerial"] | "";
  config.farmId = doc["farmId"] | "";
  config.zoneId = doc["zoneId"] | "";
  config.mqttHost = doc["mqtt"]["host"] | "";
  config.mqttPort = doc["mqtt"]["port"] | 1883;
  config.mqttClientId = doc["mqtt"]["clientId"] | "";
  config.fingerprint = doc["mqtt"]["fingerprint"] | "";
  config.configVersion = doc["configVersion"] | 1;
  config.telemetryIntervalSec = doc["telemetryIntervalSec"] | 60;
  config.samplesPerInterval = doc["samplesPerInterval"] | 1;
  config.pollingIntervalSeconds = doc["pollingIntervalSeconds"] | 60;
  config.heartbeatIntervalSeconds = doc["heartbeatIntervalSeconds"] | 15;
  config.responseTimeoutMs = doc["responseTimeoutMs"] | 3000;
  config.retryCount = doc["retryCount"] | 2;
  config.nodes.clear();
  for (JsonObject n : doc["nodes"].as<JsonArray>()) {
    NodeRoute route;
    route.deviceId = n["deviceId"] | "";
    route.hardwareAddress = n["hardwareAddress"] | "";
    route.loraAddress = n["loraAddress"] | 0;
    route.deviceType = n["deviceType"] | "SensorNode";
    if (!route.deviceId.isEmpty() && route.loraAddress > 0) config.nodes.push_back(route);
  }
  return !config.gatewayId.isEmpty() && !config.zoneId.isEmpty() && !config.mqttHost.isEmpty() &&
         !config.mqttClientId.isEmpty() && validHex64(config.fingerprint);
}
bool saveConfig(const String& raw) {
  StaticJsonDocument<8192> doc;
  DeserializationError parseError = deserializeJson(doc, raw);
  if (parseError) { Serial.printf("CONFIG_ERR:JSON_%s\n", parseError.c_str()); return false; }
  const String gatewayId = doc["gatewayId"] | "";
  const String zoneId = doc["zoneId"] | "";
  const String clientId = doc["mqtt"]["clientId"] | "";
  if (gatewayId.isEmpty()) { Serial.println("CONFIG_ERR:MISSING_GATEWAY_ID"); return false; }
  if (zoneId.isEmpty()) { Serial.println("CONFIG_ERR:MISSING_ZONE_ID"); return false; }
  if (clientId.isEmpty()) { Serial.println("CONFIG_ERR:MISSING_MQTT_CLIENT_ID"); return false; }
  const int configVersion = doc["configVersion"] | 1;
  if (configVersion < 1) { Serial.println("CONFIG_ERR:INVALID_CONFIG_VERSION"); return false; }
  const int pollingInterval = doc["pollingIntervalSeconds"] | 60;
  const int heartbeatInterval = doc["heartbeatIntervalSeconds"] | 15;
  const int responseTimeout = doc["responseTimeoutMs"] | 3000;
  const int retryCount = doc["retryCount"] | 2;
  if (pollingInterval < 10 || heartbeatInterval < 5 || responseTimeout < 500 || retryCount < 0 || retryCount > 5) {
    Serial.println("CONFIG_ERR:RUNTIME_LIMITS"); return false;
  }
  const String fingerprint = doc["mqtt"]["fingerprint"] | "";
  if (!validHex64(fingerprint)) { Serial.printf("CONFIG_ERR:FINGERPRINT_LENGTH_%u\n", (unsigned)fingerprint.length()); return false; }
  prefs.putString("config", raw);
  if (!loadConfig()) { Serial.println("CONFIG_ERR:RELOAD_FAILED"); return false; }
  return true;
}
void clearConfig() {
  mqtt.disconnect();
  prefs.remove("config");
  config = GatewayConfig();
  Serial.println("CONFIG_CLEARED");
}
bool saveWifiCommand(const String& command) {
  if (!command.startsWith("WIFI|")) return false;
  int separator = command.indexOf('|', 5);
  if (separator <= 5 || separator >= (int)command.length() - 1) return false;
  String ssid = command.substring(5, separator);
  String password = command.substring(separator + 1);
  if (ssid.isEmpty() || password.length() < 8) return false;
  prefs.putString("wifiSsid", ssid);
  prefs.putString("wifiPassword", password);
  return true;
}
bool saveEnrollmentCommand(const String& command) {
  if (!command.startsWith("ENROLL|")) return false;
  int first = command.indexOf('|', 7);
  int second = command.indexOf('|', first + 1);
  if (first <= 7 || second <= first + 1 || second >= (int)command.length() - 1) return false;
  String backendUrl = command.substring(7, first);
  String enrollmentId = command.substring(first + 1, second);
  String token = command.substring(second + 1);
  backendUrl.trim(); enrollmentId.trim(); token.trim();
  if (backendUrl.isEmpty() || enrollmentId.isEmpty() || token.isEmpty()) return false;
  prefs.putString("backendUrl", backendUrl);
  prefs.putString("enrollmentId", enrollmentId);
  prefs.putString("bootstrapToken", token);
  return true;
}
bool verifyEnrollmentHandshake() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String backendUrl = prefs.getString("backendUrl", "");
  String enrollmentId = prefs.getString("enrollmentId", "");
  String token = prefs.getString("bootstrapToken", "");
  if (backendUrl.isEmpty() || enrollmentId.isEmpty() || token.isEmpty()) {
    Serial.println("ENROLLMENT_CONFIG_REQUIRED");
    return false;
  }
  HTTPClient http;
  String url = backendUrl + "/api/v1/gateway-enrollment/" + enrollmentId + "/handshake";
  if (!http.begin(url)) { Serial.println("ENROLLMENT_HTTP_INIT_FAILED"); return false; }
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<512> doc;
  doc["macAddress"] = WiFi.macAddress();
  doc["chipId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["bootstrapToken"] = token;
  String payload; serializeJson(doc, payload);
  int status = http.POST(payload);
  String response = http.getString();
  http.end();
  if (status >= 200 && status < 300) {
    enrollmentHandshakeVerified = true;
    prefs.remove("bootstrapToken");
    Serial.printf("ENROLLMENT_HANDSHAKE_OK status=%d\n", status);
    return true;
  }
  Serial.printf("ENROLLMENT_HANDSHAKE_FAILED status=%d response=%s\n", status, response.c_str());
  return false;
}
void printConfigStatus() {
  Serial.printf("CONFIG %s | gatewayId=%s | zoneId=%s | nodes=%u\n", config.gatewayId.isEmpty() ? "MISSING" : "VALID", config.gatewayId.c_str(), config.zoneId.c_str(), (unsigned)config.nodes.size());
}
void connectWiFi() {
  // Wi-Fi credentials remain local device settings and are intentionally not part of the backend export.
  String ssid = prefs.getString("wifiSsid", "");
  String password = prefs.getString("wifiPassword", "");
  if (ssid.isEmpty()) { Serial.println("WIFI_CONFIG_REQUIRED"); return; }
  WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), password.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  Serial.println(WiFi.status() == WL_CONNECTED ? "WIFI_CONNECTED" : "WIFI_FAILED");
}
void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || config.mqttHost.isEmpty()) return;
  mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
  // The telemetry envelope contains several readings and is larger than the
  // PubSubClient default 256-byte packet buffer.
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMqttMessage);
  Serial.print("MQTT_CONNECTING host=");
  Serial.print(config.mqttHost);
  Serial.print(" port=");
  Serial.print(config.mqttPort);
  Serial.print(" clientId=");
  Serial.println(config.mqttClientId);
  String topic = "smartfarm/gateways/" + config.gatewayId + "/commands";
  if (mqtt.connect(config.mqttClientId.c_str())) {
    mqtt.subscribe(topic.c_str());
    Serial.println("MQTT_CONNECTED");
  } else {
    Serial.print("MQTT_FAILED rc=");
    Serial.println(mqtt.state());
  }
}
void publishTelemetry(const NodeRoute& node, float soil, float temperature, float humidity, float ph, uint8_t battery) {
  if (!mqtt.connected()) return;
  StaticJsonDocument<1024> doc;
  doc["messageId"] = String(config.gatewayId + "-" + node.deviceId + "-" + String(millis()));
  doc["deviceId"] = node.deviceId;
  doc["zoneId"] = config.zoneId;
  const String capturedAtUtc = utcNow();
  if (capturedAtUtc.isEmpty()) {
    Serial.println("TELEMETRY_SKIPPED:TIME_NOT_SYNCED");
    return;
  }
  doc["capturedAtUtc"] = capturedAtUtc;
  JsonArray readings = doc.createNestedArray("readings");
  JsonObject r1 = readings.createNestedObject(); r1["parameterCode"] = "SoilMoisture"; r1["value"] = soil; r1["unit"] = "%";
  JsonObject r2 = readings.createNestedObject(); r2["parameterCode"] = "Temperature"; r2["value"] = temperature; r2["unit"] = "°C";
  JsonObject r3 = readings.createNestedObject(); r3["parameterCode"] = "AirHumidity"; r3["value"] = humidity; r3["unit"] = "%";
  String topic = "smartfarm/gateways/" + config.gatewayId + "/telemetry";
  String payload; serializeJson(doc, payload);
  const bool published = mqtt.publish(topic.c_str(), payload.c_str());
  Serial.print(published ? "TELEMETRY_PUBLISHED node=" : "TELEMETRY_PUBLISH_FAILED node=");
  Serial.print(node.hardwareAddress);
  Serial.print(" bytes=");
  Serial.print(payload.length());
  if (!published) {
    Serial.print(" mqttState=");
    Serial.print(mqtt.state());
    Serial.print(" connected=");
    Serial.print(mqtt.connected() ? "1" : "0");
  }
  Serial.println();
}
void pollNode(const NodeRoute& node) {
  Serial.print("NODE_POLL address="); Serial.print(node.loraAddress); Serial.print(" id="); Serial.println(node.hardwareAddress);
  uint8_t request[4] = {0xAA, 0x10, 0, 0};
  uint16_t crc = calculateCRC16(request, 2); request[2] = crc >> 8; request[3] = crc & 0xFF;
  // Preserve unsolicited node heartbeats for the receive parser.
  sendE32Frame(node.loraAddress, request, sizeof(request));
  uint8_t buffer[80] = {}; uint8_t length = 0; unsigned long start = millis();
  while (millis() - start < NODE_REPLY_TIMEOUT_MS) {
    while (e32Serial.available() && length < sizeof(buffer)) {
      uint8_t byteIn = e32Serial.read();
      Serial.printf("[GW_RX_RAW] 0x%02X\n", byteIn);
      buffer[length] = byteIn;
      length++;
    }
    for (uint8_t i = 0; i + 13 <= length; i++) {
      if (buffer[i] != 0xAA || buffer[i + 1] != 0x11) continue;
      uint16_t expected = calculateCRC16(&buffer[i], 11);
      uint16_t actual = ((uint16_t)buffer[i + 11] << 8) | buffer[i + 12];
      if (expected != actual) continue;
      float soil = (((uint16_t)buffer[i + 3] << 8) | buffer[i + 4]) / 10.0f;
      float temperature = ((int16_t)(((uint16_t)buffer[i + 5] << 8) | buffer[i + 6])) / 10.0f;
      float humidity = (((uint16_t)buffer[i + 7] << 8) | buffer[i + 8]) / 10.0f;
      float ph = (((uint16_t)buffer[i + 9] << 8) | buffer[i + 10]) / 100.0f;
      Serial.print("NODE_POLL_OK id="); Serial.println(node.hardwareAddress);
      publishTelemetry(node, soil, temperature, humidity, ph, buffer[i + 2]);
      return;
    }
    delay(5);
  }
  Serial.print("NODE_POLL_TIMEOUT id="); Serial.println(node.hardwareAddress);
}
void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200); delay(500); prefs.begin("smartfarm", false);
  pinMode(PIN_E32_M0, OUTPUT);
  pinMode(PIN_E32_M1, OUTPUT);
  digitalWrite(PIN_E32_M0, LOW);
  digitalWrite(PIN_E32_M1, LOW);
  delay(100);
  e32Serial.begin(9600, SERIAL_8N1, PIN_E32_RX, PIN_E32_TX);
  checkAndConfigureE32();
  if (loadConfig()) printConfigStatus(); else Serial.println("CONFIG_REQUIRED");
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("NTP_SYNC_REQUESTED");
    verifyEnrollmentHandshake();
  }
  connectMqtt();
}
void loop() {
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!serialLine.isEmpty()) {
        if (serialLine == "WIFI?" || serialLine == "HELP") {
          Serial.println("WIFI|SSID|PASSWORD");
          Serial.println("CONFIG=<one-line JSON from SmartFarm>");
          Serial.println("ENROLL|BACKEND_URL|ENROLLMENT_ID|BOOTSTRAP_TOKEN");
          Serial.println("CONFIG_CLEAR");
        } else if (serialLine == "CONFIG_CLEAR") {
          clearConfig();
        } else if (saveWifiCommand(serialLine)) {
          Serial.println("WIFI_SAVED_RESTART_REQUIRED");
        } else if (saveEnrollmentCommand(serialLine)) {
          Serial.println("ENROLLMENT_SAVED_RESTART_REQUIRED");
        } else {
          String candidate = serialLine;
          if (candidate.startsWith("CONFIG=")) candidate = candidate.substring(7);
          if (saveConfig(candidate)) {
            mqtt.disconnect();
            Serial.println("CONFIG_SAVED_RESTART_REQUIRED");
            printConfigStatus();
          } else {
            Serial.println("CONFIG_INVALID");
          }
        }
        /* The previous firmware accepted only raw JSON. Keep the explicit
           CONFIG= form documented by HELP, and do not let MQTT retries block
           serial reconfiguration. */
        /* legacy branch intentionally removed */
        /* } else if (saveConfig(serialLine)) {
          Serial.println("CONFIG_SAVED_RESTART_REQUIRED");
        } else {
          Serial.println("CONFIG_INVALID");
        } */
        serialLine = "";
      }
    }
    else serialLine += c;
  }
  if (!mqtt.connected() && millis() - lastMqttAttempt >= 5000) {
    lastMqttAttempt = millis();
    connectMqtt();
  }
  mqtt.loop();
  pollRuntimeUpdate();
  const unsigned long pollIntervalMs = (unsigned long)max(1, config.pollingIntervalSeconds) * 1000UL;
  if (millis() - lastPublish > pollIntervalMs && !config.nodes.empty()) {
    lastPublish = millis();
    if (currentNodeIndex >= config.nodes.size()) currentNodeIndex = 0;
    pollNode(config.nodes[currentNodeIndex++]);
  }
}
