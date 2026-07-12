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

# 2. Tạo file CSV mới
filename = f"data_do_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"

with open(filename, mode='w', newline='', encoding='utf-8') as file:
    writer = csv.writer(file)
    
    # Ghi dòng tiêu đề
    writer.writerow(['Time(ms)', 'IR', 'RED', 'BPM', 'SpO2', 'Motion']) 
    print(f"📁 Đang lưu dữ liệu vào file: {filename}")
    print("🛑 Nhấn Ctrl+C để dừng và lưu file.\n")
    
    try:
        while True:
            # Đọc 1 dòng từ Serial (timeout=1s)
            raw_line = ser.readline()
            
            if not raw_line:
                continue
                
            line = raw_line.decode('utf-8', errors='ignore').strip()
            
            # IN RA TẤT CẢ MỌI THỨ NHẬN ĐƯỢC ĐỂ DEBUG
            print(f"Nhận được: {line}")
            
            # Định dạng mới: Time, IR, RED, BPM, SpO2, Motion -> có 5 dấu phẩy
            if line.count(',') == 5 and not line.startswith('['):
                # Tách các giá trị bằng dấu phẩy
                data = line.split(',')
                
                # Ghi vào file CSV
                writer.writerow(data)
                file.flush() # Đảm bảo ghi ngay lập tức không bị crash mất dữ liệu
                print(" -> Đã lưu vào CSV!")
                    
    except KeyboardInterrupt:
        print("\n🛑 Đã dừng ghi. File đã được lưu an toàn!")
    finally:
        ser.close()
