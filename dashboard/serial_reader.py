"""
Serial reader — reads JSON lines from the master ESP via USB serial and
writes JSON command lines back. Every line is a JSON object with a `"type"`
field so the GUI can dispatch.

Two output queues:
  - data_queue: parsed JSON dicts (consumed by the dashboard panels)
  - raw_queue:  every raw decoded line (consumed by the Raw Data window)
"""

import json
import threading
import queue
import serial


class SerialReader:
    def __init__(self, port, baudrate=115200, data_queue=None, raw_queue=None):
        self.port = port
        self.baudrate = baudrate
        self.data_queue = data_queue or queue.Queue()
        self.raw_queue = raw_queue  # may be None — raw view is optional
        self._running = False
        self._thread = None
        self._serial = None
        self._write_lock = threading.Lock()

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        if self._serial and self._serial.is_open:
            self._serial.close()

    def send_command(self, cmd_dict):
        """Write a JSON command line to the master ESP."""
        if self._serial is None or not self._serial.is_open:
            print("send_command: serial not open")
            return False
        try:
            line = json.dumps(cmd_dict) + "\n"
            with self._write_lock:
                self._serial.write(line.encode("utf-8"))
            # Echo our own command into the raw stream so users can see
            # what was sent in the Raw Data window.
            self._publish_raw(f">> {line.rstrip()}")
            return True
        except serial.SerialException as e:
            print(f"send_command failed: {e}")
            return False

    def _publish_raw(self, line):
        if self.raw_queue is None:
            return
        try:
            self.raw_queue.put_nowait(line)
        except queue.Full:
            pass

    def _read_loop(self):
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=1)
            print(f"Connected to {self.port} at {self.baudrate} baud")
            self._publish_raw(f"** connected to {self.port} **")
        except serial.SerialException as e:
            print(f"Failed to open {self.port}: {e}")
            self._publish_raw(f"** open failed: {e} **")
            self._running = False
            return

        while self._running:
            try:
                line = self._serial.readline().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                # Push the verbatim line to the raw stream regardless of
                # whether it parses as JSON. Non-JSON ESP-IDF log lines are
                # useful in the raw view for debugging.
                self._publish_raw(line)

                if not line.startswith("{"):
                    continue
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "type" not in data:
                    data["type"] = "vitals"
                self.data_queue.put(data)
            except serial.SerialException:
                print("Serial connection lost")
                self._publish_raw("** serial connection lost **")
                self._running = False
                break

    @property
    def is_running(self):
        return self._running
