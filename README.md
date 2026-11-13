# SmartFarm Firmware

Repository riêng cho firmware SmartFarm. Backend không compile firmware trong runtime; GitHub Actions tạo artifact và GitHub Release quản lý version.

## Thành phần

| Folder | Dùng cho | Cách nạp |
|---|---|---|
| `esp32-gateway/bootstrap` | Điền Wi-Fi, lưu Enrollment ID/Bootstrap Token, gửi handshake | USB bằng Arduino IDE/PlatformIO |
| `esp32-gateway/ota` | ESP32 Gateway runtime, MQTT, Node Registry, nhận OTA | USB lần đầu; OTA sau Activation |
| `arduino-nano-node` | Arduino Nano Node, Node ID, LoRa và cảm biến | USB bằng Arduino IDE/PlatformIO |

## Bootstrap ESP32

Mở `esp32-gateway/bootstrap` bằng Arduino IDE, chọn board ESP32-C3 và nạp qua USB.

Serial Monitor: `115200 baud`.

```text
WIFI|SSID|PASSWORD
ENROLL|BACKEND_URL|ENROLLMENT_ID|BOOTSTRAP_TOKEN
STATUS
```

Bootstrap không chứa production MQTT credential hoặc Tenant/Farm identity. Token được xóa khỏi NVS sau handshake thành công.

## Release

```powershell
git add .
git commit -m "Prepare SmartFarm firmware release"
git push origin main
git tag v1.0.0
git push origin v1.0.0
```

GitHub Actions sẽ tạo:

- `smartfarm-gateway-bootstrap-v1.0.0.bin`
- `smartfarm-gateway-ota-v1.0.0.bin`
- `smartfarm-node-nano-v1.0.0.hex`
- `SHA256SUMS.txt`

Sau khi Release được tạo, GitHub Webhook sẽ import release vào SmartFarm ở trạng thái `PendingReview`. Admin kiểm tra checksum rồi mới Approved.
