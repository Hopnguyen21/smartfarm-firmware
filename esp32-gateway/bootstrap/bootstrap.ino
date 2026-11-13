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

Preferences preferences;
String serialLine;

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
  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) { delay(500); Serial.print('.'); }
  Serial.println();
  Serial.println(WiFi.status() == WL_CONNECTED ? "WIFI_CONNECTED" : "WIFI_FAILED");
  if (WiFi.status() == WL_CONNECTED) handshake();
}

void setup() {
  Serial.begin(115200);
  preferences.begin("smartfarm", false);
  Serial.println("SMARTFARM_BOOTSTRAP_READY");
  connectWifi();
}

void loop() {
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
