"""Monitor PTT firmware serial output."""
import serial
import time
import sys

PORT = "COM3"
BAUD = 115200

print(f"Connecting to {PORT} at {BAUD}...")
ser = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)

# Reset board
print("Resetting board...")
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)

print("Monitoring output (Ctrl+C to stop)...\n")
print("=" * 70)

duration = int(sys.argv[1]) if len(sys.argv) > 1 else 30

start = time.time()
try:
    while time.time() - start < duration:
        if ser.in_waiting:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='replace')
            print(text, end='', flush=True)
        else:
            time.sleep(0.02)
except KeyboardInterrupt:
    pass

print("\n" + "=" * 70)
ser.close()
print("Done.")
