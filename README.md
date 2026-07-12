# HealthSense Firmware

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
- **Thành phần khác:** Nút bấm (Chân D1), LED báo hiệu (Chân D0)

### Cấu trúc dự án
Dự án được chia thành các module độc lập:
- `BLEManager`: Quản lý kết nối và truyền dữ liệu qua BLE.
- `PPGManager`: Xử lý dữ liệu từ cảm biến MAX30102.
- `AccelManager`: Xử lý dữ liệu chuyển động từ MPU6050.
- `DisplayPower`: Điều khiển nguồn và hiển thị (LED).
- `DeviceStateManager`: Quản lý trạng thái thiết bị và chế độ tiết kiệm pin.

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
- **Other Components:** Push button (D1), Status LED (D0)

### Project Structure
The project is modularized for easy maintenance:
- `BLEManager`: Manages BLE connection and data transmission.
- `PPGManager`: Processes data from the MAX30102 sensor.
- `AccelManager`: Processes motion data from the MPU6050.
- `DisplayPower`: Controls power distribution and display (LED).
- `DeviceStateManager`: Manages device states and power-saving modes.

### Installation and Usage
1. Open the project folder in VS Code with the **PlatformIO IDE** extension installed.
2. PlatformIO will automatically install the required dependencies (specified in `platformio.ini`).
3. Connect your XIAO ESP32C3 board to the computer.
4. Click the **Build** and **Upload** buttons in the PlatformIO toolbar.
