"""
ESP-NOW Sensor Network Dashboard
Usage: python main.py [serial_port]
Example: python main.py /dev/ttyUSB0
         python main.py COM3
"""

import sys
import queue
from serial_reader import SerialReader
from gui import DashboardGUI


def main():
    # Default serial port — change for your system
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        # Common defaults
        import platform
        if platform.system() == "Darwin":
            port = "/dev/tty.usbserial-0001"
        elif platform.system() == "Windows":
            port = "COM3"
        else:
            port = "/dev/ttyUSB0"

    print(f"Starting dashboard — serial port: {port}")
    print("Pass a different port as argument if needed: python main.py /dev/ttyUSB0")

    # Shared queue between serial reader and GUI
    data_queue = queue.Queue()

    # Start serial reader
    reader = SerialReader(port=port, baudrate=115200, data_queue=data_queue)
    reader.start()

    # Start GUI (blocks until window closed)
    gui = DashboardGUI(data_queue=data_queue)
    try:
        gui.run()
    finally:
        reader.stop()
        print("Dashboard closed.")


if __name__ == "__main__":
    main()
