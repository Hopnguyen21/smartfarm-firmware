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
 *   STATUS
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>

Preferences preferences;
String serialLine;
WebServer provisioningServer(80);
bool provisioningMode = false;

String chipId() { return String((uint32_t)ESP.getEfuseMac(), HEX); }

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

String provisioningPage(const String& message = "") {
  String html = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>SmartFarm Provisioning</title><style>body{font-family:Arial;max-width:520px;margin:30px auto;padding:0 16px;background:#f4f7f6;color:#17211d}input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 14px;border:1px solid #bbb;border-radius:6px}button{padding:12px 18px;background:#079669;color:white;border:0;border-radius:6px;font-weight:bold}small{color:#667}</style></head><body><h2>SmartFarm Gateway Setup</h2><p>Nhập cấu hình cho ESP32 Gateway. Thiết bị sẽ khởi động lại sau khi lưu.</p>");
  if (!message.isEmpty()) { html += "<p><b>" + message + "</b></p>"; }
  html += F("<form method='post' action='/save'><label>Wi-Fi SSID</label><input name='ssid' required><label>Wi-Fi Password</label><input name='password' type='password'><label>Backend URL</label><input name='backend' placeholder='https://api.example.com' required><label>Enrollment ID</label><input name='enrollment' required><label>Bootstrap Token</label><input name='token' type='password' required><button type='submit'>Lưu và khởi động lại</button></form><p><small>MAC: ");
  html += WiFi.macAddress();
  html += F("</small></p></body></html>");
  return html;
}

void startProvisioningPortal() {
  provisioningMode = true;
  String apName = "SMARTFARM-" + chipId().substring(8);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  provisioningServer.on("/", HTTP_GET, []() { provisioningServer.send(200, "text/html", provisioningPage()); });
  provisioningServer.on("/save", HTTP_POST, []() {
    if (!provisioningServer.hasArg("ssid") || !provisioningServer.hasArg("backend") || !provisioningServer.hasArg("enrollment") || !provisioningServer.hasArg("token")) {
      provisioningServer.send(400, "text/html", provisioningPage("Thiếu thông tin bắt buộc."));
      return;
    }
    preferences.putString("wifiSsid", provisioningServer.arg("ssid"));
    preferences.putString("wifiPassword", provisioningServer.arg("password"));
    preferences.putString("backendUrl", provisioningServer.arg("backend"));
    preferences.putString("enrollmentId", provisioningServer.arg("enrollment"));
    preferences.putString("bootstrapToken", provisioningServer.arg("token"));
    provisioningServer.send(200, "text/html", provisioningPage("Đã lưu. ESP32 sẽ khởi động lại."));
    delay(800);
    ESP.restart();
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
  if (ssid.isEmpty()) { Serial.println("WIFI_CONFIG_REQUIRED"); startProvisioningPortal(); return; }
  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) { delay(500); Serial.print('.'); }
  Serial.println();
  Serial.println(WiFi.status() == WL_CONNECTED ? "WIFI_CONNECTED" : "WIFI_FAILED");
  if (WiFi.status() != WL_CONNECTED) startProvisioningPortal();
  if (WiFi.status() == WL_CONNECTED) handshake();
}

void setup() {
  Serial.begin(115200);
  preferences.begin("smartfarm", false);
  Serial.println("SMARTFARM_BOOTSTRAP_READY");
  connectWifi();
}

void loop() {
  if (provisioningMode) provisioningServer.handleClient();
  while (Serial.available()) {
    const char character = (char)Serial.read();
    if (character == '\n' || character == '\r') {
      if (serialLine.startsWith("WIFI|")) saveWifi(serialLine);
      else if (serialLine.startsWith("ENROLL|")) saveEnrollment(serialLine);
      else if (serialLine == "RESET_WIFI") resetWifi();
      else if (serialLine == "STATUS") Serial.printf("MAC=%s CHIP=%s\n", WiFi.macAddress().c_str(), chipId().c_str());
      serialLine = "";
    } else serialLine += character;
  }
}
