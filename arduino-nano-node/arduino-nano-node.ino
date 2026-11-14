/*
 * ======================================================================================
 * SMARTFARM IOT TUTORIAL & FIRMWARE - COMBO NODE (SENSOR + ACTUATOR)
 * ======================================================================================
 * Target Board: Arduino Uno R3 or Arduino Nano (ATmega328P, 5V Logic)
 * Radio Module: EBYTE E32-Series LoRa (e.g. E32-433T20D / E32-433T30D / E32-915T20D)
 *
 * SECTION 1: HARDWARE WIRING SƠ ĐỒ CHÂN THEO BẢNG ĐẤU NỐI MỚI
 * --------------------------------------------------------------------------------------
 * Arduino Pin          Module / Component                  Description
 * --------------------------------------------------------------------------------------
 * 5V                   VCC (DHT, Relay, Cảm biến đất)      Nguồn 5V DC chung cho cảm biến & rơ-le
 * 3.3V                 VCC (E32 LoRa)                      Nguồn 3.3V DC (BẮT BUỘC KHÔNG CẮM 5V)
 * GND                  GND (Tất cả thiết bị)               Nối chung tất cả Mass (GND)
 *
 * D2                   DHT11 / DHT22 (DATA)                Chân tín hiệu cảm biến nhiệt độ & độ ẩm
 * D3 (Soft RX) <---    E32 LoRa (TXD)                      Nhận serial từ LoRa E32 sang Arduino
 * D4 (Soft TX) --->    E32 LoRa (RXD)                      Arduino truyền serial sang E32
 * D5           --->    Relay 5V (IN / S)                   Tín hiệu điều khiển bật/tắt rơ-le
 * D6           --->    E32 LoRa (M0)                       Chế độ M0 (Mức LOW = Chế độ Normal)
 * D7           --->    E32 LoRa (M1)                       Chế độ M1 (Mức LOW = Chế độ Normal)
 * D8                   (Không dùng / Bỏ trống)             AUX không cắm dây (rút gọn dây)
 * A0           <---    Cảm biến độ ẩm đất (AO)             Tín hiệu Analog (0 - 5V)
 * D13                  Onboard LED                         Đèn LED trạng thái hoạt động
 * ======================================================================================
 */
#include <SoftwareSerial.h>
#include <DHT.h>
// ======================================================================================
// SECTION 2: [USER CONFIGURABLE SECTION] - CẤU HÌNH NODE (NODE 1 HOẶC NODE 2)
// ======================================================================================
// Chọn 1 cho Node 1 (Zone 1), chọn 2 cho Node 2 (Zone 2)
#define NODE_ID_SELECTOR 1  
// Chọn loại DHT bạn đang dùng (DHT11 vỏ xanh hoặc DHT22 vỏ trắng)
#define DHTTYPE DHT11
// --- 2.1 Node Identity & Radio Domain ---
const uint8_t LORA_NETWORK_ID = 0x11;              // Khớp với Gateway Network ID!
const uint8_t LORA_CHANNEL    = 0x1A;              // 0x1A = 436.0 MHz (Khớp với Gateway)
// Cấu hình cố định chạy duy nhất Node 1
const uint8_t MY_NODE_ADDRESS = 0x02;              
const char* HARDWARE_ADDRESS  = "NODE-ZONE1-02";
// --- 2.2 Sơ đồ chân theo bảng đấu nối mới ---
const int PIN_TEMP_HUMID    = 2;   // D2: Cảm biến DHT11/DHT22 DATA
const int PIN_E32_RX        = 3;   // D3: Arduino RX (nối chân TXD của E32)
const int PIN_E32_TX        = 4;   // D4: Arduino TX (nối chân RXD của E32)
const int PIN_RELAY_CH1     = 5;   // D5: Tín hiệu IN của Module Relay 5V
const int PIN_E32_M0        = 6;   // D6: Chân M0 của LoRa E32
const int PIN_E32_M1        = 7;   // D7: Chân M1 của LoRa E32
const int PIN_SOIL_ANALOG   = A0;  // A0: Cảm biến độ ẩm đất AO
const int PIN_LED_STATE     = 13;  // D13: Onboard LED
// --- 2.3 Cấu hình Relay ---
const int RELAY_ON_LEVEL  = LOW;   
const int RELAY_OFF_LEVEL = HIGH;  
// Giới hạn thời gian relay chạy tự ngắt bảo vệ
const unsigned long MAX_SAFE_DURATION_SECONDS = 1800; // Tối đa 30 phút tự ngắt
const unsigned long DEFAULT_RUN_DURATION_SEC  = 15;   // Mặc định 15 giây nếu duration = 0
// --- 2.4 Căn chỉnh ADC Cảm biến đất ---
const int SOIL_ADC_DRY_AIR   = 750; // Giá trị ADC khi khô ngoài không khí
const int SOIL_ADC_WET_WATER = 280; // Giá trị ADC khi nhúng nước
// ======================================================================================
// SECTION 3: [HỆ THỐNG ĐIỀU KHIỂN & TRUYỀN THÔNG]
// ======================================================================================
SoftwareSerial e32Serial(PIN_E32_RX, PIN_E32_TX);
DHT dht(PIN_TEMP_HUMID, DHTTYPE);
// Trạng thái Relay
bool          g_isRelayActive = false;
unsigned long g_relayCutoffMillis = 0;
uint8_t       g_activeChannel = 1;
// --- 3.1 Tính toán CRC16 CCITT (Polynomial 0x1021) ---
uint16_t calculateCRC16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= ((uint16_t)data[i] << 8);
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}
// --- 3.2 Đợi E32 rảnh ---
void waitE32Ready() {
  delay(25);
}
// --- 3.3 Chuyển chế độ hoạt động LoRa E32 ---
void setE32ModeNormal() {
  digitalWrite(PIN_E32_M0, LOW);
  digitalWrite(PIN_E32_M1, LOW);
  delay(50);
}
void setE32ModeSleep() {
  digitalWrite(PIN_E32_M0, HIGH);
  digitalWrite(PIN_E32_M1, HIGH);
  delay(50);
}
// --- 3.4 Kiểm tra và tự động cấu hình phần cứng LoRa E32 lúc khởi động ---
bool checkE32HardwareStatus() {
  Serial.println(F("\n[TEST 2/3] Kiem tra va dong bo cau hinh LoRa E32..."));
  // Đưa E32 vào chế độ Sleep/Config (M0=1, M1=1) để đọc thông số
  setE32ModeSleep();
  while (e32Serial.available()) e32Serial.read();
  // 1. Đọc thông số hiện tại: 0xC1 0xC1 0xC1
  uint8_t cmd[3] = { 0xC1, 0xC1, 0xC1 };
  e32Serial.write(cmd, 3);
  e32Serial.flush();
  unsigned long start = millis();
  uint8_t resp[6];
  uint8_t bytesRead = 0;
  while (millis() - start < 600 && bytesRead < 6) {
    if (e32Serial.available()) {
      resp[bytesRead++] = e32Serial.read();
    }
    delay(2);
  }
  // Nếu địa chỉ E32 chưa đúng với MY_NODE_ADDRESS hoặc chưa đúng kênh/mode
  if (bytesRead >= 6 && (resp[1] != LORA_NETWORK_ID || resp[2] != MY_NODE_ADDRESS || resp[3] != 0x1A || resp[4] != LORA_CHANNEL || resp[5] != 0xC4)) {
    Serial.println(F("  -> Phat hien dia chi LoRa chua dong bo, dang ghi dia chi moi vao EEPROM E32..."));
    uint8_t setCmd[6] = { 0xC0, LORA_NETWORK_ID, MY_NODE_ADDRESS, 0x1A, LORA_CHANNEL, 0xC4 };
    while (e32Serial.available()) e32Serial.read();
    e32Serial.write(setCmd, 6);
    e32Serial.flush();
    delay(100);
    // Đọc lại để xác nhận
    bytesRead = 0;
    start = millis();
    while (millis() - start < 600 && bytesRead < 6) {
      if (e32Serial.available()) {
        resp[bytesRead++] = e32Serial.read();
      }
      delay(2);
    }
  }
  // Chuyển E32 về Normal Mode (M0=0, M1=0)
  setE32ModeNormal();
  if (bytesRead >= 4 && (resp[0] == 0xC0 || resp[0] == 0xC2)) {
    Serial.println(F("  -> [OK] LoRa E32 da san sang!"));
    Serial.print(F("  -> Phan hoi tu LoRa: "));
    for (uint8_t i = 0; i < bytesRead; i++) {
      Serial.print(F("0x"));
      if (resp[i] < 0x10) Serial.print(F("0"));
      Serial.print(resp[i], HEX);
      Serial.print(F(" "));
    }
    Serial.println();
    return true;
  } else {
    Serial.println(F("  -> [CANH BAO] Khong nhan duoc phan hoi tu LoRa E32!"));
    Serial.println(F("     Kiem tra: Nguon 3.3V/GND, D3 (RX) <-> TXD E32, D4 (TX) <-> RXD E32, D6 (M0), D7 (M1)."));
    return false;
  }
}
// --- 3.5 Kiểm tra cảm biến thực tế lúc khởi động ---
void checkSensorsInitialStatus() {
  Serial.println(F("\n[TEST 1/3] Kiem tra doc cam bien ban dau..."));
  // 1. Kiểm tra cảm biến độ ẩm đất
  int rawSoil = analogRead(PIN_SOIL_ANALOG);
  float soilMoist = map(rawSoil, SOIL_ADC_DRY_AIR, SOIL_ADC_WET_WATER, 0, 100);
  soilMoist = constrain(soilMoist, 0.0f, 100.0f);
  Serial.print(F("  - Cam bien Do am dat (A0): ADC = "));
  Serial.print(rawSoil);
  Serial.print(F(" -> Do am = "));
  Serial.print(soilMoist, 1);
  Serial.println(F("%"));
  // 2. Kiểm tra cảm biến DHT (D2)
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println(F("  - Cam bien DHT (D2): [CHUA KET NOI HOAC LOI DOC] (Khong co du lieu)"));
  } else {
    Serial.print(F("  - Cam bien DHT (D2): [OK] Nhiet do = "));
    Serial.print(t, 1);
    Serial.print(F(" *C | Do am KK = "));
    Serial.print(h, 1);
    Serial.println(F("%"));
  }
  // 3. Kiểm tra Relay (D5)
  Serial.println(F("[TEST 3/3] Kiem tra Relay (D5)..."));
  Serial.println(F("  - Trang thai Relay khoi tao: NGAT (OFF)"));
}
// --- 3.6 Điều khiển Relay ---
void turnRelayOn(uint8_t channel, unsigned long durationSeconds) {
  if (durationSeconds == 0) durationSeconds = DEFAULT_RUN_DURATION_SEC;
  if (durationSeconds > MAX_SAFE_DURATION_SECONDS) durationSeconds = MAX_SAFE_DURATION_SECONDS;
  digitalWrite(PIN_RELAY_CH1, RELAY_ON_LEVEL);
  digitalWrite(PIN_LED_STATE, HIGH);
  g_isRelayActive = true;
  g_activeChannel = channel;
  g_relayCutoffMillis = millis() + (durationSeconds * 1000UL);
  Serial.print(F("[RELAY] -> BAT (ON) trong "));
  Serial.print(durationSeconds);
  Serial.println(F(" giay."));
}
void turnRelayOff(uint8_t channel) {
  digitalWrite(PIN_RELAY_CH1, RELAY_OFF_LEVEL);
  digitalWrite(PIN_LED_STATE, LOW);
  g_isRelayActive = false;
  g_relayCutoffMillis = 0;
  Serial.println(F("[RELAY] -> TAT (OFF)."));
}
// --- 3.7 Phản hồi kết quả điều khiển Relay về Gateway ---
void sendActuatorAck(uint8_t channel, uint8_t observedState, uint8_t status) {
  uint8_t ackPayload[7];
  ackPayload[0] = 0xAA;
  ackPayload[1] = 0x21; // FRAME_ACTUATOR_ACK
  ackPayload[2] = channel;
  ackPayload[3] = observedState; // 1: Bật, 0: Tắt
  ackPayload[4] = status;        // 0: Thành công
  uint16_t crc = calculateCRC16(ackPayload, 5);
  ackPayload[5] = (uint8_t)(crc >> 8);
  ackPayload[6] = (uint8_t)(crc & 0xFF);
  waitE32Ready();
  // Định tuyến tới Gateway: [NetworkId, 0x00, Channel]
  e32Serial.write(LORA_NETWORK_ID);
  e32Serial.write((uint8_t)0x00);        // Gateway luôn có địa chỉ 0x00
  e32Serial.write(LORA_CHANNEL);
  e32Serial.write(ackPayload, sizeof(ackPayload));
  e32Serial.flush();
  Serial.println(F("[LORA TX] Da gui phan hoi Relay Ack toi Gateway."));
}
// Khởi động cảm biến ban đầu
// Missing readings are represented explicitly; never publish invented values.
float g_lastSoil = NAN;
float g_lastTemp = NAN;
float g_lastHum  = NAN;
unsigned long g_lastSensorReadMillis = 0;
void updateSensorReadingsBackground() {
  if (millis() - g_lastSensorReadMillis > 2000 || g_lastSensorReadMillis == 0) {
    g_lastSensorReadMillis = millis();
    int rawSoil = analogRead(PIN_SOIL_ANALOG);
    g_lastSoil = map(rawSoil, SOIL_ADC_DRY_AIR, SOIL_ADC_WET_WATER, 0, 100);
    g_lastSoil = constrain(g_lastSoil, 0.0f, 100.0f);
    float readHum = dht.readHumidity();
    float readTemp = dht.readTemperature();
    if (!isnan(readHum) && !isnan(readTemp)) {
      g_lastHum = readHum;
      g_lastTemp = readTemp;
    } else {
      g_lastHum = NAN;
      g_lastTemp = NAN;
      Serial.println(F("[CAM BIEN] DHT khong co du lieu"));
    }
  }
}
// --- 3.8 Lấy dữ liệu cảm biến (Phản hồi tức thì, không bị block thời gian) ---
void acquireSensorReadings(float &soil, float &temp, float &hum, float &ph, uint8_t &bat) {
  soil = g_lastSoil;
  temp = g_lastTemp;
  hum  = g_lastHum;
  // Nano firmware does not include a pH probe or battery gauge.
  ph   = NAN;
  bat  = 255; // 255 means unavailable in the wire protocol.
  Serial.print(F("[CAM BIEN] Dat: "));
  if (isnan(soil)) Serial.print(F("N/A")); else Serial.print(soil, 1);
  Serial.print(F("% | Nhiet do: "));
  if (isnan(temp)) Serial.print(F("N/A")); else Serial.print(temp, 1);
  Serial.print(F(" *C | Do am KK: "));
  if (isnan(hum)) Serial.print(F("N/A")); else Serial.print(hum, 1);
  Serial.println(F("%"));
}
// --- 3.9 Gửi gói Telemetry về Gateway qua LoRa ---
void sendTelemetryResponse(float soil, float temp, float hum, float ph, uint8_t bat) {
  uint16_t soilInt = (uint16_t)(soil * 10.0f);
  int16_t  tempInt = isnan(temp) ? (int16_t)-32768 : (int16_t)(temp * 10.0f);
  uint16_t humInt  = isnan(hum) ? (uint16_t)0xFFFF : (uint16_t)(hum * 10.0f);
  uint16_t phInt   = isnan(ph) ? (uint16_t)0xFFFF : (uint16_t)(ph * 100.0f);
  // Telemetry Payload (13 bytes):
  // [0xAA] [0x11] [Bat] [Soil_H] [Soil_L] [Temp_H] [Temp_L] [Hum_H] [Hum_L] [pH_H] [pH_L] [CRC_H] [CRC_L]
  // Missing temperature = -32768; missing humidity/pH = 0xFFFF; battery 0xFF.
  uint8_t payload[13];
  payload[0]  = 0xAA;
  payload[1]  = 0x11; // FRAME_TELEMETRY
  payload[2]  = bat;
  payload[3]  = (uint8_t)(soilInt >> 8);
  payload[4]  = (uint8_t)(soilInt & 0xFF);
  payload[5]  = (uint8_t)(tempInt >> 8);
  payload[6]  = (uint8_t)(tempInt & 0xFF);
  payload[7]  = (uint8_t)(humInt >> 8);
  payload[8]  = (uint8_t)(humInt & 0xFF);
  payload[9]  = (uint8_t)(phInt >> 8);
  payload[10] = (uint8_t)(phInt & 0xFF);
  uint16_t crc = calculateCRC16(payload, 11);
  payload[11] = (uint8_t)(crc >> 8);
  payload[12] = (uint8_t)(crc & 0xFF);
  waitE32Ready();
  // Gửi đích danh Gateway: [NetworkId, 0x00, Channel]
  e32Serial.write(LORA_NETWORK_ID);
  e32Serial.write((uint8_t)0x00);        // Gateway 0x00
  e32Serial.write(LORA_CHANNEL);
  e32Serial.write(payload, sizeof(payload));
  e32Serial.flush();
  Serial.println(F("[LORA TX] Da gui Telemetry ve Gateway!"));
}
// --- Khởi tạo Arduino ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n======================================================="));
  Serial.println(F("   SMARTFARM IOT - ARDUINO COMBO NODE INITIALIZING"));
  Serial.println(F("======================================================="));
  // Cấu hình Relay trạng thái ngắt an toàn
  pinMode(PIN_RELAY_CH1, OUTPUT);
  digitalWrite(PIN_RELAY_CH1, RELAY_OFF_LEVEL);
  pinMode(PIN_LED_STATE, OUTPUT);
  digitalWrite(PIN_LED_STATE, LOW);
  pinMode(PIN_E32_M0, OUTPUT);
  pinMode(PIN_E32_M1, OUTPUT);
  // Khởi động cảm biến DHT
  dht.begin();
  // Khởi động LoRa SoftwareSerial (9600 baud)
  e32Serial.begin(9600);
  // 1. Kiểm tra cảm biến ban đầu
  checkSensorsInitialStatus();
  // 2. Tự động kiểm tra phần cứng LoRa E32
  checkE32HardwareStatus();
  Serial.print(F("[CAU HINH] Node Serial: "));
  Serial.println(HARDWARE_ADDRESS);
  Serial.print(F("[CAU HINH] LoRa Address: 0x0"));
  Serial.println(MY_NODE_ADDRESS, HEX);
  Serial.print(F("[CAU HINH] Network ID: 0x"));
  Serial.print(LORA_NETWORK_ID, HEX);
  Serial.print(F(" | Kênh Channel: 0x"));
  Serial.println(LORA_CHANNEL, HEX);
  Serial.println(F("-------------------------------------------------------"));
  Serial.println(F("[SAN SANG] Node dang lang nghe tin nhan tu Gateway..."));
  Serial.println(F("=======================================================\n"));
}
// --- Vòng lặp chính ---
void loop() {
  // Cập nhật ngầm dữ liệu cảm biến (Non-blocking)
  updateSensorReadingsBackground();
  // 1. WATCHDOG TỰ NGẮT BẢO VỆ RELAY:
  if (g_isRelayActive && (millis() >= g_relayCutoffMillis)) {
    Serial.println(F("[WATCHDOG] Het thoi gian hen gio, tu dong ngat Relay!"));
    turnRelayOff(g_activeChannel);
    sendActuatorAck(g_activeChannel, 0, 0); // Báo cáo về Gateway đã ngắt
  }
  // 2. NHẬN GÓI TIN TỪ LORA GATEWAY:
  if (e32Serial.available()) {
    byte b = e32Serial.read();
    Serial.print(F("[RX_BYTE] 0x"));
    if (b < 0x10) Serial.print(F("0"));
    Serial.println(b, HEX);
    if (b == 0xAA) {
      Serial.println(F("\n[LORA RX] Phat hien Header 0xAA tu Gateway..."));
      unsigned long start = millis();
      while (e32Serial.available() < 1 && (millis() - start < 100)) delay(2);
      if (e32Serial.available() >= 1) {
        uint8_t cmdType = e32Serial.read();
        // ----------------------------------------------------
        // LỆNH 0x10: GATEWAY HỎI DỮ LIỆU CẢM BIẾN (POLL)
        // ----------------------------------------------------
        if (cmdType == 0x10) {
          start = millis();
          while (e32Serial.available() < 2 && (millis() - start < 150)) delay(2);
          if (e32Serial.available() >= 2) {
            uint8_t crcH = e32Serial.read();
            uint8_t crcL = e32Serial.read();
            uint8_t checkBuf[2] = { 0xAA, 0x10 };
            uint16_t expectedCrc = calculateCRC16(checkBuf, 2);
            uint16_t packetCrc = ((uint16_t)crcH << 8) | crcL;
            if (expectedCrc == packetCrc) {
              Serial.println(F("[LORA RX] Lenh POLL hop le -> Dang gui Telemetry..."));
              float soil, temp, hum, ph;
              uint8_t bat;
              acquireSensorReadings(soil, temp, hum, ph, bat);
              sendTelemetryResponse(soil, temp, hum, ph, bat);
            } else {
              Serial.println(F("[LORA RX] Sai CRC lenh POLL!"));
            }
          }
        }
        // ----------------------------------------------------
        // LỆNH 0x20: GATEWAY ĐIỀU KHIỂN RELAY (ACTUATOR)
        // ----------------------------------------------------
        else if (cmdType == 0x20) {
          while (e32Serial.available() < 6 && (millis() - start < 100)) delay(2);
          if (e32Serial.available() >= 6) {
            uint8_t channel  = e32Serial.read();
            uint8_t action   = e32Serial.read();
            uint8_t durH     = e32Serial.read();
            uint8_t durL     = e32Serial.read();
            uint8_t crcH     = e32Serial.read();
            uint8_t crcL     = e32Serial.read();
            uint8_t checkBuf[6] = { 0xAA, 0x20, channel, action, durH, durL };
            uint16_t expectedCrc = calculateCRC16(checkBuf, 6);
            uint16_t packetCrc   = ((uint16_t)crcH << 8) | crcL;
            if (expectedCrc == packetCrc) {
              uint16_t duration = ((uint16_t)durH << 8) | durL;
              Serial.println(F("\n[LORA RX] Nhan lenh dieu khien RELAY tu Gateway:"));
              if (action == 1) {
                turnRelayOn(channel, duration);
                sendActuatorAck(channel, 1, 0); // Báo cáo: Bật thành công
              } else {
                turnRelayOff(channel);
                sendActuatorAck(channel, 0, 0); // Báo cáo: Tắt thành công
              }
            }
          }
        }
      }
    }
  }
}
