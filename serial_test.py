"""Quick serial reader to verify ESP32 is outputting data."""
import serial
import time
import sys

port = "COM3"
baud = 115200

print(f"Opening {port} at {baud} baud...")
ser = serial.Serial(port, baud, timeout=1, dsrdtr=False, rtscts=False)

# Toggle DTR/RTS to reset ESP32
print("Resetting board via DTR/RTS...")
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.dtr = False
ser.rts = False
time.sleep(0.1)

print("Reading output for 5 seconds...\n")
print("=" * 60)

start = time.time()
got_data = False
while time.time() - start < 5:
    if ser.in_waiting:
        data = ser.read(ser.in_waiting)
        try:
            text = data.decode('utf-8', errors='replace')
        except:
            text = repr(data)
        print(text, end='', flush=True)
        got_data = True
    else:
        time.sleep(0.05)

print("\n" + "=" * 60)
if not got_data:
    print("NO DATA received! Trying different baud rates...")
    ser.close()
    for test_baud in [9600, 74880, 115200, 921600]:
        ser = serial.Serial(port, test_baud, timeout=1, dsrdtr=False, rtscts=False)
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(2)
        if ser.in_waiting:
            data = ser.read(ser.in_waiting)
            print(f"  {test_baud} baud: Got {len(data)} bytes: {data[:80]}")
        else:
            print(f"  {test_baud} baud: No data")
        ser.close()
else:
    print("Data received successfully!")

try:
    ser.close()
except:
    pass
