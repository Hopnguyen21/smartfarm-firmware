/* SmartFarm ESP32 Gateway Bootstrap
 *
 * USB-flash this sketch before Enrollment. It stores Wi-Fi and enrollment
 * settings in NVS, performs the one-time backend handshake, then waits for
 * the approved OTA/runtime firmware. No production MQTT credential is built
 * into this sketch.
 *
 * Serial commands at 115200:
 *   WIFI|SSID|PASSWORD
 *   ENROLL|BACKEND_URL|ENROLLMENT_ID|BOOTSTRAP_TOKEN
 *   RESET_WIFI
 *   RESET_ENROLLMENT
 *   STATUS
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Update.h>
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
// Use the Arduino IDE USB CDC serial channel for diagnostics and commands.
// Enable "USB CDC On Boot" in the IDE.

Preferences preferences;
String serialLine;
WebServer provisioningServer(80);
bool provisioningMode = false;
bool enrollmentSyncComplete = false;
unsigned long lastUpdatePoll = 0;
unsigned long lastEnrollmentAttempt = 0;
uint8_t otaApplyFailures = 0;
unsigned long otaRetryAfter = 0;
unsigned long lastConfigPoll = 0;
unsigned long lastMqttAttempt = 0;
// Backend address on the SmartFarm Wi-Fi LAN. Change only when deploying to another LAN.
const char* BACKEND_URL = "http://192.168.1.159:5144";
const char* BOOTSTRAP_FIRMWARE_VERSION = "1.1.1";

WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);
String runtimeGatewayId, runtimeMqttHost, runtimeMqttClientId;
uint16_t runtimeMqttPort = 1883;
String activeOtaCommandId;

String backendUrl() {
  return preferences.getString("backendUrl", BACKEND_URL);
}

String chipId() { return String((uint32_t)ESP.getEfuseMac(), HEX); }

void prepareOtaBoot() {
  if (!preferences.getBool("otaPending", false)) return;
  uint8_t boots = preferences.getUChar("otaBoots", 0) + 1;
  preferences.putUChar("otaBoots", boots);
  Serial.printf("OTA_PENDING_BOOT attempt=%u\n", boots);
  if (boots <= 3) return;
  const esp_partition_t* current = esp_ota_get_running_partition();
  const esp_partition_t* previous = esp_ota_get_next_update_partition(current);
  if (previous && esp_ota_set_boot_partition(previous) == ESP_OK) {
    preferences.remove("otaPending"); preferences.remove("otaBoots");
    Serial.println("OTA_ROLLBACK_RESTARTING"); delay(100); ESP.restart();
  }
}

void confirmOtaBoot() {
  if (!preferences.getBool("otaPending", false) || WiFi.status() != WL_CONNECTED) return;
  preferences.remove("otaPending"); preferences.remove("otaBoots");
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println("OTA_BOOT_CONFIRMED");
}

void saveWifi(const String& command) {
  const int split = command.indexOf('|', 5);
  if (split <= 5 || split >= (int)command.length() - 1) { Serial.println("WIFI_INVALID"); return; }
  preferences.putString("wifiSsid", command.substring(5, split));
  preferences.putString("wifiPassword", command.substring(split + 1));
  Serial.println("WIFI_SAVED_RESTART_REQUIRED");
}

void saveEnrollment(const String& command) {
  const int first = command.indexOf('|', 7);
  const int second = command.indexOf('|', first + 1);
  if (first <= 7 || second <= first + 1 || second >= (int)command.length() - 1) { Serial.println("ENROLL_INVALID"); return; }
  preferences.putString("backendUrl", command.substring(7, first));
  preferences.putString("enrollmentId", command.substring(first + 1, second));
  preferences.putString("bootstrapToken", command.substring(second + 1));
  Serial.println("ENROLLMENT_SAVED_RESTART_REQUIRED");
}

void resetWifi() {
  preferences.remove("wifiSsid");
  preferences.remove("wifiPassword");
  Serial.println("WIFI_RESET_OK_RESTARTING");
  delay(300);
  ESP.restart();
}

void resetEnrollment() {
  preferences.remove("wifiSsid");
  preferences.remove("wifiPassword");
  preferences.remove("enrolled");
  preferences.remove("otaArmed");
  preferences.remove("backendUrl");
  preferences.remove("enrollmentId");
  preferences.remove("bootstrapToken");
  Serial.println("ENROLLMENT_RESET_OK_RESTARTING");
  delay(300);
  ESP.restart();
}

void autoEnroll() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("ENROLLMENT_REQUIRES_WIFI"); return; }
  Serial.println("ENROLLMENT_START source=automatic");
  HTTPClient http;
  const String endpoint = backendUrl() + "/api/v1/gateway-enrollment/auto";
  if (!http.begin(endpoint)) { Serial.println("ENROLLMENT_BACKEND_UNAVAILABLE"); return; }
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<384> document;
  document["macAddress"] = WiFi.macAddress();
  document["chipId"] = chipId();
  document["bootstrapFirmwareVersion"] = BOOTSTRAP_FIRMWARE_VERSION;
  String payload; serializeJson(document, payload);
  const int status = http.POST(payload);
  const String response = http.getString();
  http.end();
  if (status >= 200 && status < 300) { preferences.putBool("enrolled", true); preferences.putBool("otaArmed", true); enrollmentSyncComplete = true; Serial.println("ENROLLMENT_OK"); }
  else { Serial.print("ENROLLMENT_FAILED status="); Serial.print(status); Serial.print(" body="); Serial.println(response); }
}

bool applyRuntimeUpdate(const String& url, const String& expected, const String& version) {
  Serial.print("OTA_DOWNLOAD_START version="); Serial.print(version); Serial.print(" url="); Serial.print(url); Serial.print(" sha256="); Serial.println(expected);
  HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); if (!http.begin(url)) { Serial.println("OTA_DOWNLOAD_FAILED:BEGIN"); return false; }
  const int code = http.GET(); if (code != HTTP_CODE_OK) { http.end(); return false; }
  Serial.print("OTA_DOWNLOAD_HEADERS status="); Serial.print(code); Serial.print(" bytes="); Serial.println(http.getSize());
  WiFiClient* stream = http.getStreamPtr(); mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts(&hash, 0);
  uint8_t buffer[1024]; size_t total = 0; unsigned long last = millis();
  while (http.connected() && (millis() - last < 15000)) { size_t available = stream->available(); if (!available) { delay(10); continue; } size_t read = stream->readBytes(buffer, min(available, sizeof(buffer))); if (!read) continue; mbedtls_sha256_update(&hash, buffer, read); total += read; last = millis(); }
  uint8_t digest[32]; mbedtls_sha256_finish(&hash, digest); mbedtls_sha256_free(&hash); http.end(); String actual; for (uint8_t b : digest) { if (b < 16) actual += '0'; actual += String(b, HEX); } actual.toUpperCase();
  Serial.print("OTA_DOWNLOAD_COMPLETE bytes="); Serial.print(total); Serial.print(" sha256="); Serial.println(actual);
  if (!actual.equalsIgnoreCase(expected) || total == 0) { Serial.println("OTA_SHA256_MISMATCH"); return false; }
  Serial.println("OTA_SHA256_OK");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); if (!http.begin(url) || http.GET() != HTTP_CODE_OK) { Serial.println("OTA_DOWNLOAD_FAILED:WRITE"); http.end(); return false; }
  const int contentLength = http.getSize();
  Serial.print("OTA_WRITE_BEGIN status=200 size="); Serial.println(contentLength);
  const size_t updateSize = contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize)) { Serial.print("OTA_WRITE_FAILED:BEGIN error="); Serial.println(Update.errorString()); http.end(); return false; }
  WiFiClient* writeStream = http.getStreamPtr();
  uint8_t writeBuffer[1024]; size_t written = 0; unsigned long lastWriteData = millis();
  while (http.connected() && (contentLength <= 0 || written < (size_t)contentLength) && millis() - lastWriteData < 15000) {
    const int available = writeStream->available();
    if (available <= 0) { delay(10); continue; }
    const size_t want = contentLength > 0 ? min((size_t)sizeof(writeBuffer), (size_t)contentLength - written) : min((size_t)sizeof(writeBuffer), (size_t)available);
    const size_t readCount = writeStream->readBytes(writeBuffer, want);
    if (!readCount) continue;
    const size_t accepted = Update.write(writeBuffer, readCount);
    written += accepted; lastWriteData = millis();
    if (accepted != readCount) break;
  }
  // Never mark an image bootable when Update detected an internal write or
  // validation error. The previous `true` value could reboot into a corrupt
  // image after printing OTA_APPLIED.
  // The image is already fully downloaded and verified above. Commit the
  // OTA slot so the ROM bootloader can select it after reboot.
  const bool ended = Update.end(true);
  const bool ok = written > 0 && (contentLength <= 0 || written == (size_t)contentLength) && ended && Update.isFinished();
  if (!ok) { Serial.print("OTA_WRITE_FAILED:END written="); Serial.print(written); Serial.print(" expected="); Serial.print(contentLength); Serial.print(" error="); Serial.println(Update.errorString()); }
  http.end();
  Serial.print("OTA_WRITE_COMPLETE written="); Serial.print(written); Serial.print(" expected="); Serial.println(contentLength);
  if (ok) {
    preferences.putString("pendingRuntimeVersion", version);
    preferences.putString("pendingOtaCommandId", activeOtaCommandId);
    preferences.putBool("otaPending", true);
    preferences.putUChar("otaBoots", 0);
    preferences.putBool("otaArmed", false);
    Serial.println("OTA_APPLIED");
    Serial.println("OTA_RESTART_SCHEDULED");
    Serial.flush();
    delay(1000);
    Serial.println("OTA_RESTARTING");
    Serial.flush();
    delay(100);
    esp_restart();
  }
  return ok;
}

void reportOtaResult(const String& version, const char* result, const char* errorCode = nullptr, const char* errorMessage = nullptr) {
  if (!mqtt.connected() || runtimeGatewayId.isEmpty()) return;
  StaticJsonDocument<384> doc;
  doc["commandId"] = activeOtaCommandId;
  doc["macAddress"] = WiFi.macAddress();
  doc["version"] = version;
  doc["result"] = result;
  if (errorCode) doc["errorCode"] = errorCode;
  if (errorMessage) doc["errorMessage"] = errorMessage;
  String payload; serializeJson(doc, payload);
  const String topic = "smartfarm/gateways/" + runtimeGatewayId + "/ota-result";
  mqtt.publish(topic.c_str(), payload.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String raw; raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) raw += (char)payload[i];
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, raw)) return;
  if (String(doc["type"] | "") != "OTA") return;
  const String commandId = doc["commandId"] | "";
  const String version = doc["version"] | "";
  const String firmwareUrl = doc["firmwareUrl"] | "";
  const String sha = doc["artifactSha256"] | "";
  if (commandId.isEmpty() || version.isEmpty() || firmwareUrl.isEmpty() || sha.length() != 64) return;
  if (preferences.getString("lastOtaCommand", "") == commandId) return;
  preferences.putString("lastOtaCommand", commandId);
  activeOtaCommandId = commandId;
  reportOtaResult(version, "Started");
  if (!applyRuntimeUpdate(firmwareUrl, sha, version)) {
    reportOtaResult(version, "Failed", "OTA_APPLY_FAILED", "Firmware download, checksum or flash failed");
    otaApplyFailures = min<uint8_t>(otaApplyFailures + 1, 3);
    otaRetryAfter = millis() + (otaApplyFailures == 1 ? 10000UL : otaApplyFailures == 2 ? 30000UL : 120000UL);
  } else otaApplyFailures = 0;
}

void fetchRuntimeConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  const String url = backendUrl() + "/api/v1/gateway-runtime/config?macAddress=" + WiFi.macAddress();
  if (!http.begin(url) || http.GET() != HTTP_CODE_OK) { http.end(); return; }
  StaticJsonDocument<4096> doc;
  if (!deserializeJson(doc, http.getString())) {
    runtimeGatewayId = doc["gatewayId"] | "";
    runtimeMqttHost = doc["mqtt"]["host"] | "";
    if (runtimeMqttHost.isEmpty()) runtimeMqttHost = doc["mqttHost"] | "";
    runtimeMqttPort = doc["mqtt"]["port"] | 1883;
    if (doc["mqtt"]["port"].isNull()) runtimeMqttPort = doc["mqttPort"] | 1883;
    runtimeMqttClientId = doc["mqtt"]["clientId"] | "";
    if (runtimeMqttClientId.isEmpty()) runtimeMqttClientId = doc["mqttClientId"] | "";
  }
  http.end();
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || runtimeMqttHost.isEmpty() || runtimeMqttClientId.isEmpty()) return;
  mqtt.setServer(runtimeMqttHost.c_str(), runtimeMqttPort);
  mqtt.setCallback(onMqttMessage);
  if (mqtt.connect(runtimeMqttClientId.c_str())) {
    mqtt.subscribe(("smartfarm/gateways/" + runtimeGatewayId + "/commands").c_str());
    Serial.println("MQTT_CONNECTED");
  } else Serial.printf("MQTT_FAILED rc=%d\n", mqtt.state());
}

String provisioningPage(const String& message = "") {
  String html = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>SmartFarm Provisioning</title><style>body{font-family:Arial;max-width:520px;margin:30px auto;padding:0 16px;background:#f4f7f6;color:#17211d}input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 14px;border:1px solid #bbb;border-radius:6px}button{padding:12px 18px;background:#079669;color:white;border:0;border-radius:6px;font-weight:bold;margin:4px 0}small{color:#667}</style></head><body><h2>SmartFarm Gateway Setup</h2><p>Chỉ nhập Wi-Fi. Sau khi kết nối, bấm nút để bắt đầu Enrollment.</p>");
  if (!message.isEmpty()) { html += "<p><b>" + message + "</b></p>"; }
  html += F("<form method='post' action='/save'><label>Wi-Fi SSID</label><input name='ssid' required><label>Wi-Fi Password</label><input name='password' type='password'><button type='submit'>Lưu Wi-Fi</button></form><form method='post' action='/enroll'><button type='submit'>Bắt đầu Enrollment</button></form><p><small>MAC: ");
  html += WiFi.macAddress();
  html += F("</small></p></body></html>");
  return html;
}

void startProvisioningPortal() {
  provisioningMode = true;
  String apName = "SMARTFARM-" + chipId().substring(8);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  provisioningServer.on("/", HTTP_GET, []() { provisioningServer.send(200, "text/html; charset=utf-8", provisioningPage()); });
  provisioningServer.on("/save", HTTP_POST, []() {
    if (!provisioningServer.hasArg("ssid")) {
      provisioningServer.send(400, "text/html; charset=utf-8", provisioningPage("Thiếu thông tin bắt buộc."));
      return;
    }
    preferences.putString("wifiSsid", provisioningServer.arg("ssid"));
    preferences.putString("wifiPassword", provisioningServer.arg("password"));
    provisioningServer.send(200, "text/html; charset=utf-8", provisioningPage("Đã lưu Wi-Fi. ESP32 sẽ khởi động lại."));
    delay(800);
    ESP.restart();
  });
  provisioningServer.on("/enroll", HTTP_POST, []() {
    if (WiFi.status() != WL_CONNECTED) { provisioningServer.send(409, "text/html; charset=utf-8", provisioningPage("ESP32 chưa kết nối Wi-Fi.")); return; }
    HTTPClient http;
    if (!http.begin(backendUrl() + "/api/v1/gateway-enrollment/auto")) { provisioningServer.send(500, "text/html; charset=utf-8", provisioningPage("Không mở được Backend.")); return; }
    http.addHeader("Content-Type", "application/json");
    StaticJsonDocument<384> document;
    document["macAddress"] = WiFi.macAddress();
    document["chipId"] = chipId();
    document["bootstrapFirmwareVersion"] = BOOTSTRAP_FIRMWARE_VERSION;
    String payload; serializeJson(document, payload);
    const int status = http.POST(payload);
    http.end();
    if (status < 200 || status >= 300) { provisioningServer.send(502, "text/html; charset=utf-8", provisioningPage("Enrollment thất bại. Kiểm tra thiết bị đã được cấp phát.")); return; }
    preferences.putBool("enrolled", true);
    preferences.putBool("otaArmed", true);
    provisioningServer.send(200, "text/html; charset=utf-8", provisioningPage("Enrollment thành công. Có thể đóng trang này."));
    return;
  });
  provisioningServer.begin();
  Serial.print("PROVISIONING_AP="); Serial.println(apName);
  Serial.println("PROVISIONING_URL=http://192.168.4.1");
}

bool handshake() {
  const String backend = preferences.getString("backendUrl", "");
  const String enrollment = preferences.getString("enrollmentId", "");
  const String token = preferences.getString("bootstrapToken", "");
  if (backend.isEmpty() || enrollment.isEmpty() || token.isEmpty()) { Serial.println("ENROLLMENT_CONFIG_REQUIRED"); return false; }
  HTTPClient http;
  if (!http.begin(backend + "/api/v1/gateway-enrollment/" + enrollment + "/handshake")) return false;
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<512> document;
  document["macAddress"] = WiFi.macAddress();
  document["chipId"] = chipId();
  document["bootstrapToken"] = token;
  String payload; serializeJson(document, payload);
  const int status = http.POST(payload);
  http.end();
  if (status < 200 || status >= 300) { Serial.printf("ENROLLMENT_HANDSHAKE_FAILED status=%d\n", status); return false; }
  preferences.remove("bootstrapToken");
  Serial.println("ENROLLMENT_HANDSHAKE_OK");
  return true;
}

void connectWifi() {
  const String ssid = preferences.getString("wifiSsid", "");
  const String password = preferences.getString("wifiPassword", "");
  if (ssid.isEmpty()) { Serial.println("WIFI_CONFIG_REQUIRED"); return; }
  WiFi.mode(WIFI_STA);
  Serial.print("WIFI_CONNECT_START ssid="); Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) { delay(500); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) { Serial.print("WIFI_CONNECTED ip="); Serial.print(WiFi.localIP()); Serial.print(" mac="); Serial.println(WiFi.macAddress()); }
  else Serial.println("WIFI_FAILED");
  if (WiFi.status() != WL_CONNECTED) Serial.println("WIFI_CONFIG_REQUIRED_OR_FAILED");
}

void setup() {
  Serial.begin(115200);
  preferences.begin("smartfarm", false);
  prepareOtaBoot();
  Serial.print("SMARTFARM_BOOTSTRAP_READY version="); Serial.println(BOOTSTRAP_FIRMWARE_VERSION);
  connectWifi();
  confirmOtaBoot();
  // Always reconcile enrollment with the backend after Wi-Fi is available.
  // The local NVS flag can survive a DB reset, so gating this call on
  // `enrolled` would leave the ESP silently enrolled against stale data.
  if (WiFi.status() == WL_CONNECTED) { autoEnroll(); fetchRuntimeConfig(); connectMqtt(); }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && millis() - lastConfigPoll >= 10000) { lastConfigPoll = millis(); fetchRuntimeConfig(); }
  if (!mqtt.connected() && millis() - lastMqttAttempt >= 5000) { lastMqttAttempt = millis(); connectMqtt(); }
  mqtt.loop();
  if (WiFi.status() == WL_CONNECTED && !enrollmentSyncComplete && millis() - lastEnrollmentAttempt >= 10000) { lastEnrollmentAttempt = millis(); autoEnroll(); }
  if (provisioningMode) provisioningServer.handleClient();
  while (Serial.available()) {
    const char character = (char)Serial.read();
    if (character == '\n' || character == '\r') {
      if (serialLine.startsWith("WIFI|")) saveWifi(serialLine);
      else if (serialLine.startsWith("ENROLL|")) saveEnrollment(serialLine);
      else if (serialLine == "RESET_WIFI") resetWifi();
      else if (serialLine == "RESET_ENROLLMENT") resetEnrollment();
      else if (serialLine == "START_ENROLL") autoEnroll();
      else if (serialLine == "STATUS") Serial.printf("MAC=%s CHIP=%s\n", WiFi.macAddress().c_str(), chipId().c_str());
      serialLine = "";
    } else serialLine += character;
  }
}
