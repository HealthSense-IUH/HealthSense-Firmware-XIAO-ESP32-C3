import os
import serial
import serial.tools.list_ports
import csv
import time
from datetime import datetime

# 1. Tìm cổng COM tự động
ports = serial.tools.list_ports.comports()
if not ports:
    print("❌ Không tìm thấy cổng COM nào. Hãy cắm mạch vào máy tính!")
    exit(1)

print("🔍 Các cổng COM đang cắm vào máy:")
for i, p in enumerate(ports):
    print(f"  [{i}] {p.device} - {p.description}")

if len(ports) > 1:
    idx = input(f"👉 Chọn số thứ tự cổng COM của ESP32 (0 đến {len(ports)-1}) [Mặc định 0]: ")
    idx = int(idx) if idx.strip().isdigit() else 0
    PORT = ports[idx].device
else:
    PORT = ports[0].device
BAUDRATE = 115200

print(f"🔌 Đang kết nối với mạch qua cổng {PORT}...")

try:
    # Cấu hình cổng COM nhưng chưa mở
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUDRATE
    ser.timeout = 1
    # Bật DTR/RTS (rất quan trọng cho mạch ESP32-C3 Native USB)
    ser.dtr = True
    ser.rts = True
    ser.open()
    
    time.sleep(1) # Đợi mạch khởi động
    print("✅ Kết nối thành công! Đang lắng nghe dữ liệu...")
except Exception as e:
    print(f"❌ Lỗi kết nối: {e}")
    exit(1)

# 2. Tạo thư mục data nếu chưa có
if not os.path.exists('data'):
    os.makedirs('data')

print("⏳ Chờ thiết bị bắt đầu đo... (mỗi lần đo sẽ được lưu vào 1 file riêng)")
print("🛑 Nhấn Ctrl+C để dừng.\n")

# --- Quản lý session ---
current_file = None
current_writer = None
current_filename = None
session_count = 0

def open_new_session():
    """Mở file CSV mới cho một lần đo."""
    global current_file, current_writer, current_filename, session_count
    session_count += 1
    current_filename = f"data/do_{datetime.now().strftime('%Y%m%d_%H%M%S')}_session{session_count}.csv"
    current_file = open(current_filename, mode='w', newline='', encoding='utf-8')
    current_writer = csv.writer(current_file)
    current_writer.writerow(['Time(ms)', 'IR', 'RED', 'BPM', 'SpO2', 'Motion'])
    print(f"\n📂 [Session {session_count}] Bắt đầu lưu vào: {current_filename}")

def close_current_session(reason=""):
    """Đóng file CSV của session hiện tại."""
    global current_file, current_writer, current_filename
    if current_file is not None:
        current_file.flush()
        current_file.close()
        print(f"✅ Đã lưu xong file: {current_filename}{' (' + reason + ')' if reason else ''}")
        current_file = None
        current_writer = None
        current_filename = None

try:
    while True:
        # Đọc 1 dòng từ Serial (timeout=1s)
        raw_line = ser.readline()

        if not raw_line:
            continue

        line = raw_line.decode('utf-8', errors='ignore').strip()

        # IN RA TẤT CẢ MỌI THỨ NHẬN ĐƯỢC ĐỂ DEBUG
        print(f"Nhận được: {line}")

        # --- Phát hiện KẾT THÚC session ---
        # Firmware in "[SCREENING] Xong 1 phut" khi hết 60s đo
        # Firmware in "[UNWEAR] Da thao dong ho" khi tháo thiết bị
        if "[SCREENING] Xong 1 phut" in line or "[UNWEAR]" in line:
            if current_file is not None:
                reason = "hết 60s đo" if "[SCREENING]" in line else "tháo thiết bị"
                close_current_session(reason)
            continue

        # --- Ghi dữ liệu PPG vào file ---
        # Định dạng: Time, IR, RED, BPM, SpO2, Motion -> có 5 dấu phẩy
        if line.count(',') == 5 and not line.startswith('['):
            # Mở file mới nếu chưa có session nào đang mở
            if current_file is None:
                open_new_session()

            data = line.split(',')
            current_writer.writerow(data)
            current_file.flush() # Đảm bảo ghi ngay lập tức không bị crash mất dữ liệu
            print(" -> Đã lưu vào CSV!")

except KeyboardInterrupt:
    print("\n🛑 Đã dừng.")
    close_current_session("dừng thủ công")
finally:
    ser.close()
