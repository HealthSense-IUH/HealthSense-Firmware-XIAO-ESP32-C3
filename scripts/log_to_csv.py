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

# Lấy cổng COM đầu tiên tìm thấy (nếu bạn cắm nhiều thiết bị, có thể cần đổi tay)
PORT = ports[0].device
BAUDRATE = 115200

print(f"🔌 Đang kết nối với mạch qua cổng {PORT}...")

try:
    ser = serial.Serial(PORT, BAUDRATE, timeout=1)
    ser.dtr = True
    ser.rts = True
    time.sleep(2) # Đợi mạch khởi động
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
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if not line:
                continue
                
            # Bỏ qua các dòng log debug của hệ thống, chỉ lấy dòng dữ liệu
            # Định dạng mới: Time, IR, RED, BPM, SpO2, Motion -> có 5 dấu phẩy
            if line.count(',') == 5 and not line.startswith('['):
                # Tách các giá trị bằng dấu phẩy
                data = line.split(',')
                
                # Ghi vào file CSV
                writer.writerow(data)
                file.flush() # Đảm bảo ghi ngay lập tức không bị crash mất dữ liệu
                
                print(f"Lưu: {data}")
            else:
                # In ra các log khác (debug) để bạn tiện theo dõi
                print(f"Log: {line}")
                    
    except KeyboardInterrupt:
        print("\n🛑 Đã dừng ghi. File đã được lưu an toàn!")
    finally:
        ser.close()
