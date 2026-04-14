"""Interactive serial test runner for ESP32S3-AI V2.2 board."""
import serial
import time
import sys
import threading

PORT = "COM3"
BAUD = 115200

def read_serial(ser, duration=12, stop_event=None):
    """Read and print serial data for a given duration."""
    start = time.time()
    while time.time() - start < duration:
        if stop_event and stop_event.is_set():
            break
        if ser.in_waiting:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            print(text, end='', flush=True)
        else:
            time.sleep(0.02)

def main():
    print(f"Connecting to {PORT} at {BAUD}...")
    ser = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
    
    # Reset the board
    print("Resetting board...")
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)
    
    # Wait for boot and read menu
    print("Waiting for boot...\n")
    read_serial(ser, duration=4)
    
    # Run each test
    test = sys.argv[1] if len(sys.argv) > 1 else "5"
    
    if test == "menu":
        # Interactive mode
        print("\n\nType a number and press Enter to send to board (q to quit):")
        while True:
            try:
                cmd = input("> ")
                if cmd.lower() == 'q':
                    break
                ser.write(cmd.encode())
                read_serial(ser, duration=12)
            except KeyboardInterrupt:
                break
    else:
        print(f"\n--- Sending command: {test} ---\n")
        ser.write(test.encode())
        # Test 1 (buttons) and 4 (loopback) run for 10s, others shorter
        duration = 15 if test in ('1', '4', '5') else 8
        if test == '5':
            duration = 60  # all tests combined
        read_serial(ser, duration=duration)
    
    ser.close()
    print("\n\nDone!")

if __name__ == "__main__":
    main()
