"""
Serial reader module — reads JSON lines from master ESP via USB serial.
"""

import json
import threading
import queue
import serial


class SerialReader:
    def __init__(self, port, baudrate=115200, data_queue=None):
        self.port = port
        self.baudrate = baudrate
        self.data_queue = data_queue or queue.Queue()
        self._running = False
        self._thread = None
        self._serial = None

    def start(self):
        """Start reading serial data in a background thread."""
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def stop(self):
        """Stop the serial reader."""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        if self._serial and self._serial.is_open:
            self._serial.close()

    def _read_loop(self):
        """Main read loop — connects to serial and parses JSON lines."""
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=1)
            print(f"Connected to {self.port} at {self.baudrate} baud")
        except serial.SerialException as e:
            print(f"Failed to open {self.port}: {e}")
            self._running = False
            return

        while self._running:
            try:
                line = self._serial.readline().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                # Try to parse as JSON (master sends JSON lines)
                if line.startswith("{"):
                    try:
                        data = json.loads(line)
                        self.data_queue.put(data)
                    except json.JSONDecodeError:
                        pass  # Skip non-JSON lines (ESP-IDF log messages)
            except serial.SerialException:
                print("Serial connection lost")
                self._running = False
                break

    @property
    def is_running(self):
        return self._running
