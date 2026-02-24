# nsh_shell.py
import serial
import threading

# --- Configuration ---
COM_PORT = 'COM6'  # Replace with your COM port
BAUD = 115200

print(f"Connecting to Spresense on {COM_PORT} at {BAUD} baud...")

# Open serial port
ser = serial.Serial(COM_PORT, BAUD, timeout=0.1)

# --- Function to continuously read from Spresense ---
def read_from_board():
    while True:
        try:
            data = ser.read(1024)
            if data:
                print(data.decode(errors='ignore'), end='', flush=True)
        except Exception as e:
            print(f"Error reading: {e}")
            break

# Start the reading thread
thread = threading.Thread(target=read_from_board, daemon=True)
thread.start()

# --- Main loop to send commands ---
try:
    while True:
        cmd = input("NSH> ")
        if cmd.lower() in ('exit', 'quit'):
            print("Exiting shell...")
            break
        # Send command with newline
        ser.write((cmd + '\n').encode())
except KeyboardInterrupt:
    print("\nExiting shell...")

# Close serial port
ser.close()