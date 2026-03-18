"""
Dashboard GUI — displays live sensor data from both sender nodes.
Uses tkinter (comes with Python, no extra install needed).
"""

import tkinter as tk
from tkinter import ttk
import queue


class DashboardGUI:
    def __init__(self, data_queue):
        self.data_queue = data_queue
        self.root = tk.Tk()
        self.root.title("ESP-NOW Sensor Network Dashboard")
        self.root.geometry("800x500")
        self.root.configure(bg="#1e1e1e")

        # Data storage for each node
        self.node_data = {1: {}, 2: {}}
        self.labels = {1: {}, 2: {}}

        self._build_ui()

    def _build_ui(self):
        """Build the dashboard layout with two panels."""
        title = tk.Label(self.root, text="Sensor Network Dashboard",
                         font=("Helvetica", 18, "bold"), fg="#ffffff", bg="#1e1e1e")
        title.pack(pady=10)

        # Container for both panels
        container = tk.Frame(self.root, bg="#1e1e1e")
        container.pack(fill=tk.BOTH, expand=True, padx=20)

        # Create panel for each sender node
        for node_id in [1, 2]:
            frame = tk.LabelFrame(container, text=f"  Sender Node {node_id}  ",
                                  font=("Helvetica", 14, "bold"),
                                  fg="#00ccff", bg="#2d2d2d",
                                  labelanchor="n", padx=15, pady=10)
            frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=10, pady=5)

            fields = [
                ("Heart Rate", "hr", "BPM"),
                ("SpO2", "spo2", "%"),
                ("Body Temp", "body_temp", "C"),
                ("Env Temp", "env_temp", "C"),
                ("Gas PPM", "gas_ppm", "PPM"),
                ("Peer RSSI", "rssi_peer", "dBm"),
            ]

            for i, (label_text, key, unit) in enumerate(fields):
                row = tk.Frame(frame, bg="#2d2d2d")
                row.pack(fill=tk.X, pady=3)

                name_label = tk.Label(row, text=f"{label_text}:",
                                      font=("Helvetica", 11), fg="#aaaaaa",
                                      bg="#2d2d2d", width=12, anchor="w")
                name_label.pack(side=tk.LEFT)

                value_label = tk.Label(row, text="--",
                                       font=("Helvetica", 11, "bold"), fg="#ffffff",
                                       bg="#2d2d2d", width=10, anchor="e")
                value_label.pack(side=tk.LEFT)

                unit_label = tk.Label(row, text=unit,
                                      font=("Helvetica", 9), fg="#666666",
                                      bg="#2d2d2d", anchor="w")
                unit_label.pack(side=tk.LEFT, padx=(5, 0))

                self.labels[node_id][key] = value_label

        # Status bar
        self.status_label = tk.Label(self.root, text="Waiting for data...",
                                     font=("Helvetica", 10), fg="#666666", bg="#1e1e1e")
        self.status_label.pack(pady=5)

    def _update_data(self):
        """Poll the data queue and update labels."""
        count = 0
        while not self.data_queue.empty() and count < 10:
            try:
                data = self.data_queue.get_nowait()
                node_id = data.get("node", 0)
                if node_id in self.labels:
                    self.node_data[node_id] = data
                    for key, label in self.labels[node_id].items():
                        value = data.get(key, "--")
                        if isinstance(value, float):
                            label.config(text=f"{value:.1f}")
                        else:
                            label.config(text=str(value))

                        # Color alerts
                        if key == "gas_ppm" and isinstance(value, (int, float)) and value > 200:
                            label.config(fg="#ff4444")
                        elif key == "gas_ppm":
                            label.config(fg="#44ff44")

                    self.status_label.config(text=f"Receiving data from Node {node_id}",
                                             fg="#44ff44")
                count += 1
            except queue.Empty:
                break

        # Schedule next update
        self.root.after(100, self._update_data)

    def run(self):
        """Start the GUI main loop."""
        self._update_data()
        self.root.mainloop()
