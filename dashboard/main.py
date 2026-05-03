"""
Wearable Vitals Monitoring Dashboard
Usage: python main.py [serial_port]
Example: python main.py /dev/ttyUSB0
         python main.py COM3
"""

import sys
import queue
from serial_reader import SerialReader
from gui import DashboardGUI


def main():
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        import platform
        if platform.system() == "Darwin":
            port = "/dev/tty.usbserial-0001"
        elif platform.system() == "Windows":
            port = "COM3"
        else:
            port = "/dev/ttyUSB0"

    print(f"Starting dashboard — serial port: {port}")
    print("Pass a different port as argument if needed: python main.py /dev/ttyUSB0")

    # data_queue: parsed JSON for the panels
    # raw_queue:  every raw line for the Raw Data window
    data_queue = queue.Queue()
    raw_queue = queue.Queue(maxsize=4096)

    reader = SerialReader(port=port, baudrate=115200,
                          data_queue=data_queue, raw_queue=raw_queue)
    reader.start()

    gui = DashboardGUI(data_queue=data_queue, raw_queue=raw_queue, reader=reader)
    try:
        gui.run()
    finally:
        reader.stop()
        print("Dashboard closed.")


if __name__ == "__main__":
    main()
