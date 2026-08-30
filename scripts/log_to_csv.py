import os
import serial
import serial.tools.list_ports
import csv
import time
from datetime import datetime

# ==============================================================================
# CONFIGURATION CONSTANTS
# ==============================================================================
DEFAULT_BAUDRATE = 115200
SERIAL_TIMEOUT_SEC = 1
DATA_DIR = "data"
CSV_HEADER = ['Time(ms)', 'IR', 'RED', 'BPM', 'SpO2', 'Motion']

# ==============================================================================
# MESSAGE CONSTANTS
# ==============================================================================
MSG_NO_PORTS_FOUND = "[ERROR] No COM ports found. Please connect your device to the computer."
MSG_AVAILABLE_PORTS_HEADER = "[INFO] Available COM ports:"
MSG_SELECT_PORT_PROMPT = "[INPUT] Select COM port index (0 to {max_idx}) [Default 0]: "
MSG_CONNECTING = "[INFO] Connecting to device on {port} (Baudrate: {baudrate})..."
MSG_CONNECTED = "[INFO] Connected successfully! Listening for data..."
MSG_CONNECT_ERROR = "[ERROR] Connection failed: {error}"
MSG_WAITING_DATA = "[INFO] Waiting for measurement data... (Each session will be saved to a separate CSV file)"
MSG_STOP_HINT = "[INFO] Press Ctrl+C to stop.\n"
MSG_SESSION_START = "[SESSION {session_id}] Started logging to: {filename}"
MSG_SESSION_SAVED = "[SESSION] Closed file: {filename}{reason_str}"
MSG_RECORD_SAVED = " -> Saved record to CSV"
MSG_PROCESS_STOPPED = "\n[INFO] Process stopped by user."

# Protocol / Trigger strings from Firmware
TAG_SCREENING = "[SCREENING]"
TAG_UNWEAR = "[UNWEAR]"
KEYWORDS_DONE = ("thành công", "hoàn tất", "thanh cong", "hoan tat")
KEYWORDS_ABORT = ("Ngừng đo", "Ngung do")

REASON_PHASE_DONE = "phase completed"
REASON_MOTION_ABORT = "aborted due to motion"
REASON_DEVICE_UNWEAR = "device unstrapped"
REASON_MANUAL_STOP = "manual stop"


# ==============================================================================
# SESSION MANAGEMENT
# ==============================================================================
current_file = None
current_writer = None
current_filename = None
session_count = 0


def open_new_session():
    """Open a new CSV file for a measurement session."""
    global current_file, current_writer, current_filename, session_count
    session_count += 1
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    current_filename = os.path.join(DATA_DIR, f"measurement_{timestamp}_session{session_count}.csv")
    current_file = open(current_filename, mode='w', newline='', encoding='utf-8')
    current_writer = csv.writer(current_file)
    current_writer.writerow(CSV_HEADER)
    print(MSG_SESSION_START.format(session_id=session_count, filename=current_filename))


def close_current_session(reason=""):
    """Close the current session's CSV file."""
    global current_file, current_writer, current_filename
    if current_file is not None:
        current_file.flush()
        current_file.close()
        reason_str = f" ({reason})" if reason else ""
        print(MSG_SESSION_SAVED.format(filename=current_filename, reason_str=reason_str))
        current_file = None
        current_writer = None
        current_filename = None


# ==============================================================================
# MAIN ROUTINE
# ==============================================================================
def main():
    # 1. Discover available COM ports
    ports = serial.tools.list_ports.comports()
    if not ports:
        print(MSG_NO_PORTS_FOUND)
        exit(1)

    print(MSG_AVAILABLE_PORTS_HEADER)
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device} - {p.description}")

    if len(ports) > 1:
        idx_raw = input(MSG_SELECT_PORT_PROMPT.format(max_idx=len(ports) - 1))
        idx = int(idx_raw) if idx_raw.strip().isdigit() and 0 <= int(idx_raw) < len(ports) else 0
        port_name = ports[idx].device
    else:
        port_name = ports[0].device

    print(MSG_CONNECTING.format(port=port_name, baudrate=DEFAULT_BAUDRATE))

    try:
        ser = serial.Serial()
        ser.port = port_name
        ser.baudrate = DEFAULT_BAUDRATE
        ser.timeout = SERIAL_TIMEOUT_SEC
        # Enable DTR/RTS (Required for ESP32-C3 Native USB)
        ser.dtr = True
        ser.rts = True
        ser.open()
        time.sleep(1)
        print(MSG_CONNECTED)
    except Exception as e:
        print(MSG_CONNECT_ERROR.format(error=e))
        exit(1)

    # 2. Ensure data directory exists
    if not os.path.exists(DATA_DIR):
        os.makedirs(DATA_DIR)

    print(MSG_WAITING_DATA)
    print(MSG_STOP_HINT)

    try:
        while True:
            raw_line = ser.readline()
            if not raw_line:
                continue

            line = raw_line.decode('utf-8', errors='ignore').strip()
            print(f"[RAW] {line}")

            # Check for completion or abort signals from firmware
            is_phase_done = TAG_SCREENING in line and any(kw in line for kw in KEYWORDS_DONE)
            is_phase_aborted = TAG_SCREENING in line and any(kw in line for kw in KEYWORDS_ABORT)
            is_unwear = TAG_UNWEAR in line

            if is_phase_done or is_phase_aborted or is_unwear:
                if current_file is not None:
                    if is_phase_done:
                        reason = REASON_PHASE_DONE
                    elif is_phase_aborted:
                        reason = REASON_MOTION_ABORT
                    else:
                        reason = REASON_DEVICE_UNWEAR
                    close_current_session(reason)
                continue

            # Record sample data (Format: Time, IR, RED, BPM, SpO2, Motion -> 5 commas)
            if line.count(',') == 5 and not line.startswith('['):
                if current_file is None:
                    open_new_session()

                data = line.split(',')
                current_writer.writerow(data)
                current_file.flush()
                print(MSG_RECORD_SAVED)

    except KeyboardInterrupt:
        print(MSG_PROCESS_STOPPED)
        close_current_session(REASON_MANUAL_STOP)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
