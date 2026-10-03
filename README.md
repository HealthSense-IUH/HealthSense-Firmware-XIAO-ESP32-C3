# HealthSense Firmware For Seeed XIAO ESP32-C3

## Tiếng Việt

**HealthSense Firmware** là phần mềm nhúng (firmware) dành cho thiết bị theo dõi sức khỏe đeo tay, được phát triển trên nền tảng PlatformIO.

### Chức năng chính
- Đo và theo dõi nhịp tim (BPM) và nồng độ oxy trong máu (SpO2) qua cảm biến MAX30102.
- Phát hiện chuyển động của người dùng sử dụng cảm biến gia tốc MPU6050.
- Phát hiện trạng thái tháo thiết bị khỏi tay để tiết kiệm năng lượng.
- Truyền dữ liệu cảm biến không dây thông qua Bluetooth Low Energy (BLE).
- Quản lý năng lượng thông minh với chế độ Deep Sleep.

### Phần cứng
- **Vi điều khiển:** Seeed Studio XIAO ESP32C3
- **Cảm biến nhịp tim:** MAX30102 (Giao tiếp I2C, Ngắt ở chân D6)
- **Cảm biến chuyển động:** MPU6050 (Giao tiếp I2C, Ngắt ở chân D2, Nguồn cấp ở chân D10)
- **Thành phần khác:** Nút bấm (Chân D3), LED NeoPixel (Chân D0), đo pin qua cầu phân áp 2 x 100K (Chân D1)
- Toàn bộ chân và ngưỡng khai báo ở `src/config.h`.

### Cấu trúc dự án
```
src/
  main.cpp                 Khởi tạo module, vòng loop điều phối luồng dữ liệu theo chế độ
  config.h                 Chân kết nối, thời gian, ngưỡng, cờ log Serial
  modules/
    DeviceStateManager     Máy trạng thái: chế độ, sự kiện, chu kỳ sàng lọc AFib 10 phút
    PPGManager             MAX30102: đọc FIFO, tính BPM / SpO2, gom gói PPG, phát hiện tháo tay
    AccelManager           MPU6050: ngắt vung tay, đếm bước
    BLEManager             BLE GATT: gửi dữ liệu, nhận lệnh, mức pin, bộ đệm offline
    BleProtocol.h          Giao thức với app: UUID, lệnh, định dạng gói
    DisplayPower           LED NeoPixel và nguồn MPU6050
scripts/log_to_csv.py      Ghi dòng CSV từ Serial ra data/*.csv (mỗi lần đo một file)
```

### Kiến trúc
Vòng `loop()` (không dùng RTOS task riêng, mọi việc chạy tuần tự, ngắt chỉ bật cờ):
1. `DeviceStateManager_handleButton`: nhấn ngắn bật LED 3 giây, giữ 4 giây tắt máy.
2. `AccelManager`: đếm bước 25Hz; ngắt vung tay thành sự kiện `EVT_MOTION`.
3. `PPGManager`: có ngắt dữ liệu thì đọc FIFO, cập nhật BPM / SpO2, gom gói 10 mẫu; IR thấp liên tục 2 giây thành `EVT_NOT_WEARING`.
4. Luồng dữ liệu theo chế độ: WORKOUT gửi `W:` mỗi 3 giây (mất kết nối thì ghi đệm); MEASURE / SCREENING gửi gói PPG thô.
5. `DeviceStateManager_loop`: đổi chế độ (exit -> enter), dò đeo tay khi IDLE, chạy chu kỳ sàng lọc.
6. `BLEManager_loop`: gửi bù dữ liệu tập luyện đã ghi đệm sau khi kết nối lại.
7. Cập nhật mức pin mỗi 30 giây.

Chế độ:
```
TURN_ON -> IDLE --(đang đeo)--------------------> SCREENING (chu kỳ 10 phút: pha 1 đo 60s -> R1,
          IDLE <-(tháo tay / CMD:IDLE)--------- MEASURE    pha 2-4 ở 2,5 / 5 / 7,5 phút đo 30s -> R2)
          IDLE <-(rung tay khi MEASURE)-------- WORKOUT    (CMD:WORKOUT_STOP về IDLE, 10 phút sau đó không tự sàng lọc)
Giữ nút 4 giây ở bất kỳ chế độ nào -> SHUTDOWN (deep sleep; giữ nút 3 giây để bật lại)
```

BLE (chi tiết trong `BleProtocol.h`): một service với 3 characteristic DATA (notify: PPG thô, sự kiện `CMD:` / `ACK:` / `ERR:`), COMMAND (write: lệnh từ app) và REPORT (notify: `R1:` / `R2:` / `W:` / dữ liệu gửi bù), cùng Battery Service chuẩn 0x180F.

### Cài đặt và Sử dụng
1. Mở thư mục dự án bằng VS Code có cài đặt tiện ích **PlatformIO IDE**.
2. PlatformIO sẽ tự động tải các thư viện cần thiết (được khai báo trong `platformio.ini`).
3. Kết nối board XIAO ESP32C3 với máy tính.
4. Nhấn nút **Build** và **Upload** trên thanh công cụ của PlatformIO.

---

## English

**HealthSense Firmware** is the embedded software for a wearable health monitoring device, developed using PlatformIO.

### Key Features
- Measures and monitors Heart Rate (BPM) and Blood Oxygen Saturation (SpO2) via the MAX30102 sensor.
- Detects user motion using the MPU6050 accelerometer.
- Detects device removal (not wearing) to save power.
- Transmits sensor data wirelessly via Bluetooth Low Energy (BLE).
- Smart power management featuring Deep Sleep mode.

### Hardware Requirements
- **Microcontroller:** Seeed Studio XIAO ESP32C3
- **Heart Rate Sensor:** MAX30102 (I2C interface, Interrupt on D6)
- **Motion Sensor:** MPU6050 (I2C interface, Interrupt on D2, Power on D10)
- **Other Components:** Push button (D3), NeoPixel status LED (D0), battery sense via a 2 x 100K divider (D1)
- All pins and thresholds live in `src/config.h`.

### Project Structure
- `main.cpp`: initializes the modules and routes data per device mode each loop.
- `config.h`: pins, timings, thresholds and Serial log switches.
- `DeviceStateManager`: state machine (modes, events, 10-minute AFib screening cycle).
- `PPGManager`: MAX30102 FIFO reading, BPM / SpO2, PPG packets, removal detection.
- `AccelManager`: MPU6050 motion interrupt and step counting.
- `BLEManager` + `BleProtocol.h`: BLE GATT service, commands, battery level, offline buffer; the protocol shared with the phone app.
- `DisplayPower`: NeoPixel LED and MPU6050 power.

See the Vietnamese "Kiến trúc" section above for the loop, mode diagram and BLE layout.

### Installation and Usage
1. Open the project folder in VS Code with the **PlatformIO IDE** extension installed.
2. PlatformIO will automatically install the required dependencies (specified in `platformio.ini`).
3. Connect your XIAO ESP32C3 board to the computer.
4. Click the **Build** and **Upload** buttons in the PlatformIO toolbar.
