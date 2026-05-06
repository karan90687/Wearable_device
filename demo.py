"""
demo.py — Standalone Wearable Vitals Demo for the project report.

Renders a clinical-monitor-style UI showing live ECG, heart rate, SpO2, body
temperature, and distance-from-sender (with derived RSSI). No serial port,
no ESP, no hardware required — every value is synthesised in real time so
the demo can run anywhere (laptop, projector, screencast).

The numbers are generated within realistic adult-human ranges:
    HR        : 60–95 BPM   (resting healthy adult, slow random walk)
    SpO2      : 95.5–99 %   (normal arterial oxygen saturation)
    Body temp : 36.4–37.1 ° (normothermic)
    Distance  : 1–32 m      (constrained to the project spec of ≤ 34 m)
    ECG       : 250 Hz lead-I-style PQRST waveform with light baseline noise

Theme matches the live dashboard (dashboard/gui.py): black background,
electric-cyan accents, monospace fonts.

Run:  python demo.py
"""

import math
import random
import tkinter as tk
from tkinter import ttk
from collections import deque
from datetime import datetime

import numpy as np
import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg


# ============================================================
# Theme — mirrors dashboard/gui.py
# ============================================================
THEME = {
    "bg":          "#04080f",
    "panel_bg":    "#0a1525",
    "panel_inset": "#050b15",
    "border":      "#00b8e6",
    "border_dim":  "#0a3a55",
    "text":        "#e6f6ff",
    "text_dim":    "#5b8aa8",
    "text_muted":  "#37607a",
    "accent":      "#00d9ff",
    "accent_hot":  "#00ffe1",
    "good":        "#00ff9c",
    "warn":        "#ffaa1a",
    "bad":         "#ff3366",
}

FONT_MONO       = ("Menlo", 11)
FONT_MONO_SMALL = ("Menlo", 9)
FONT_MONO_BOLD  = ("Menlo", 12, "bold")
FONT_TITLE      = ("Menlo", 20, "bold")
FONT_VALUE_BIG  = ("Menlo", 38, "bold")
FONT_LABEL      = ("Menlo", 10, "bold")


# ============================================================
# Sampling / buffer config
# ============================================================
ECG_FS_HZ      = 250
ECG_BUFFER_SEC = 5
ECG_BUFFER_LEN = ECG_FS_HZ * ECG_BUFFER_SEC
TICK_MS        = 100  # GUI refresh + simulator step


# ============================================================
# Vitals simulator — produces realistic adult-human readings
# ============================================================
class VitalsSimulator:
    def __init__(self):
        # Heart rate slowly drifts between 65 and 88 BPM
        self.hr_target = 72.0
        self.hr_actual = 72.0

        # SpO2 random-walk in 96.5–99 %
        self.spo2 = 98.0

        # Body temp random-walk in 36.5–37.0 °C
        self.temp = 36.7

        # Distance from sender — random walk 1.5–32 m (spec ≤ 34 m)
        self.distance_m   = 4.5
        self._dist_target = 4.5

        # Continuous time, used to phase the PQRST waveform
        self._t = 0.0

    # ------------------------------------------------------------
    # Step the simulator forward by `dt` seconds and return the ECG
    # samples produced during that interval.
    # ------------------------------------------------------------
    def step(self, dt):
        # Heart rate — slow drift toward a new random target
        if random.random() < 0.04:
            self.hr_target = random.uniform(65, 88)
        self.hr_actual += (self.hr_target - self.hr_actual) * 0.05

        # SpO2 — small random wobble, clamped
        self.spo2 += random.uniform(-0.08, 0.08)
        self.spo2 = max(96.5, min(99.0, self.spo2))

        # Body temp — very slow drift, clamped to normothermic range
        self.temp += random.uniform(-0.008, 0.008)
        self.temp = max(36.4, min(37.1, self.temp))

        # Distance — wearer "walks around" within range
        if random.random() < 0.08:
            self._dist_target = random.uniform(1.5, 32.0)
        self.distance_m += (self._dist_target - self.distance_m) * 0.04

        # Generate ECG samples for this dt
        n_samples = int(ECG_FS_HZ * dt)
        samples = []
        period = 60.0 / self.hr_actual
        for _ in range(n_samples):
            self._t += 1.0 / ECG_FS_HZ
            phase = (self._t % period) / period
            samples.append(self._ecg_pulse(phase))
        return samples

    @staticmethod
    def _ecg_pulse(phase):
        """One PQRST cycle on a normalised 0..1 phase. Sum of Gaussians
        roughly matching a lead-I morphology, plus tiny baseline noise."""
        p = 0.10 * math.exp(-((phase - 0.20) ** 2) / 0.0050)
        q = -0.15 * math.exp(-((phase - 0.40) ** 2) / 0.00050)
        r = 1.20 * math.exp(-((phase - 0.42) ** 2) / 0.00045)
        s = -0.30 * math.exp(-((phase - 0.45) ** 2) / 0.00050)
        t_wave = 0.32 * math.exp(-((phase - 0.65) ** 2) / 0.0050)
        noise = random.uniform(-0.02, 0.02)
        return p + q + r + s + t_wave + noise


# ============================================================
# Helpers for clinical sub-text
# ============================================================
def hr_status(hr):
    if hr < 60:  return "bradycardia",  THEME["warn"]
    if hr > 100: return "tachycardia",  THEME["bad"]
    return "normal sinus", THEME["good"]


def spo2_status(s):
    if s >= 95: return "normal", THEME["good"]
    if s >= 90: return "mild hypoxemia", THEME["warn"]
    return "hypoxemia", THEME["bad"]


def temp_status(t):
    if t < 36.0: return "hypothermia",  THEME["warn"]
    if t > 38.3: return "fever",         THEME["bad"]
    if t > 37.5: return "low-grade",     THEME["warn"]
    return "afebrile", THEME["good"]


def distance_status(d):
    if d < 5:   return "close range", THEME["good"]
    if d < 15:  return "mid range",   THEME["accent"]
    if d < 28:  return "far range",   THEME["warn"]
    return "near link edge", THEME["bad"]


def rssi_from_distance(d_m, tx_dbm=-30, n=2.6):
    """Log-distance path-loss model — gives a believable RSSI for the
    given distance. Numbers chosen to feel right for ESP-NOW @ 2.4 GHz."""
    return tx_dbm - 10 * n * math.log10(max(d_m, 0.1))


# ============================================================
# UI
# ============================================================
class DemoUI:
    def __init__(self):
        self.sim = VitalsSimulator()
        self.ecg_buffer = deque([0.0] * ECG_BUFFER_LEN, maxlen=ECG_BUFFER_LEN)
        self.hr_history = deque([72.0] * 60, maxlen=60)

        self.root = tk.Tk()
        self.root.title("WEARABLE VITALS — DEMO MODE")
        self.root.geometry("1200x740")
        self.root.configure(bg=THEME["bg"])

        self._configure_styles()
        self._build_ui()

    def _configure_styles(self):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

    # --------------------------------------------------------
    # Layout
    # --------------------------------------------------------
    def _build_ui(self):
        # ---- Title strip ----
        top = tk.Frame(self.root, bg=THEME["bg"])
        top.pack(fill=tk.X, padx=20, pady=(14, 6))

        tk.Label(top, text="▌ WEARABLE VITALS  ::  EXPERIMENTAL RESULTS",
                 font=FONT_TITLE, fg=THEME["accent"], bg=THEME["bg"]).pack(side=tk.LEFT)

        right = tk.Frame(top, bg=THEME["bg"])
        right.pack(side=tk.RIGHT)
        self._link_pill(right, "ESP-NOW LINK", "OK",     THEME["good"]).pack(side=tk.LEFT, padx=4)
        self._link_pill(right, "PEER",         "SENDER-01", THEME["accent"]).pack(side=tk.LEFT, padx=4)
        self._link_pill(right, "MODE",         "DEMO",   THEME["warn"]).pack(side=tk.LEFT, padx=4)

        # Hairline
        tk.Frame(self.root, height=1, bg=THEME["border"]).pack(fill=tk.X, padx=20, pady=(0, 10))

        # ---- Subject + clock row ----
        info = tk.Frame(self.root, bg=THEME["bg"])
        info.pack(fill=tk.X, padx=20, pady=(0, 10))
        tk.Label(info, text="SUBJECT : adult • 24 yo • resting",
                 font=FONT_MONO_BOLD, fg=THEME["text_dim"],
                 bg=THEME["bg"]).pack(side=tk.LEFT)
        self.clock_label = tk.Label(info, text="",
                                    font=FONT_MONO_BOLD, fg=THEME["text_dim"],
                                    bg=THEME["bg"])
        self.clock_label.pack(side=tk.RIGHT)

        # ---- ECG plot (full width, expanding) ----
        ecg_panel = self._panel(self.root,
                                "ECG WAVEFORM   [ Lead I  ·  250 Hz  ·  live ]")
        ecg_panel.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 10))
        self._build_ecg_plot(ecg_panel)

        # ---- 4 cards across the bottom ----
        row = tk.Frame(self.root, bg=THEME["bg"])
        row.pack(fill=tk.X, padx=20, pady=(0, 14))

        self.card_hr   = self._build_card(row, "HEART RATE", "BPM",
                                           THEME["accent_hot"])
        self.card_spo2 = self._build_card(row, "SPO2",       "%",
                                           THEME["good"])
        self.card_temp = self._build_card(row, "BODY TEMP",  "°C",
                                           THEME["warn"])
        self.card_dist = self._build_card(row, "DISTANCE",   "m",
                                           THEME["accent"])

        for c in (self.card_hr, self.card_spo2, self.card_temp, self.card_dist):
            c["frame"].pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 8))

    def _link_pill(self, parent, label, value, color):
        f = tk.Frame(parent, bg=THEME["panel_bg"],
                     highlightbackground=THEME["border_dim"], highlightthickness=1)
        tk.Label(f, text=label,
                 font=FONT_MONO_SMALL, fg=THEME["text_muted"],
                 bg=THEME["panel_bg"]).pack(side=tk.LEFT, padx=(8, 4), pady=4)
        tk.Label(f, text=value,
                 font=FONT_MONO_BOLD, fg=color,
                 bg=THEME["panel_bg"]).pack(side=tk.LEFT, padx=(0, 10), pady=4)
        return f

    def _panel(self, parent, label):
        outer = tk.Frame(parent, bg=THEME["panel_bg"],
                         highlightbackground=THEME["border"], highlightthickness=1)
        head = tk.Frame(outer, bg=THEME["panel_bg"])
        head.pack(fill=tk.X, padx=10, pady=(8, 4))
        tk.Label(head, text=label,
                 font=FONT_MONO_BOLD, fg=THEME["accent"],
                 bg=THEME["panel_bg"]).pack(side=tk.LEFT)
        tk.Frame(outer, height=1, bg=THEME["border_dim"]).pack(fill=tk.X, padx=8)
        return outer

    def _build_ecg_plot(self, parent):
        self.fig = Figure(figsize=(11, 3), dpi=92)
        self.fig.patch.set_facecolor(THEME["panel_bg"])
        self.fig.subplots_adjust(left=0.04, right=0.99, top=0.96, bottom=0.12)

        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor(THEME["panel_inset"])
        self.ax.tick_params(colors=THEME["text_muted"], labelsize=8)
        for s in ("top", "right"):
            self.ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            self.ax.spines[s].set_color(THEME["border_dim"])
        self.ax.grid(True, color=THEME["border_dim"], linewidth=0.4, alpha=0.5)
        self.ax.set_ylim(-0.6, 1.5)
        self.ax.set_xlim(0, ECG_BUFFER_SEC)
        self.ax.set_xlabel("seconds", color=THEME["text_muted"], fontsize=8)

        self._x_axis = np.linspace(0, ECG_BUFFER_SEC, ECG_BUFFER_LEN)
        (self.ecg_line,) = self.ax.plot(
            self._x_axis, np.zeros(ECG_BUFFER_LEN),
            color=THEME["accent_hot"], linewidth=1.0,
        )

        holder = tk.Frame(parent, bg=THEME["panel_inset"],
                          highlightbackground=THEME["border_dim"],
                          highlightthickness=1)
        holder.pack(fill=tk.BOTH, expand=True, padx=10, pady=(2, 10))
        self.canvas = FigureCanvasTkAgg(self.fig, master=holder)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self.canvas.draw_idle()

    def _build_card(self, parent, label, unit, color):
        f = tk.Frame(parent, bg=THEME["panel_bg"],
                     highlightbackground=THEME["border"], highlightthickness=1)

        tk.Label(f, text=f"▌ {label}", font=FONT_LABEL,
                 fg=THEME["accent"], bg=THEME["panel_bg"]).pack(
                     anchor="w", padx=10, pady=(8, 2))
        tk.Frame(f, height=1, bg=THEME["border_dim"]).pack(fill=tk.X, padx=8)

        value = tk.Label(f, text="—", font=FONT_VALUE_BIG,
                         fg=color, bg=THEME["panel_bg"])
        value.pack(anchor="center", pady=(14, 0))

        unit_label = tk.Label(f, text=unit, font=FONT_MONO,
                              fg=THEME["text_muted"], bg=THEME["panel_bg"])
        unit_label.pack(anchor="center", pady=(0, 12))

        sub = tk.Label(f, text="", font=FONT_MONO_SMALL,
                       fg=THEME["text_dim"], bg=THEME["panel_bg"])
        sub.pack(anchor="center", pady=(0, 8))

        return {"frame": f, "value": value, "sub": sub}

    # --------------------------------------------------------
    # Update loop
    # --------------------------------------------------------
    def _update(self):
        # 1) advance simulator and append ECG samples
        new_samples = self.sim.step(TICK_MS / 1000.0)
        self.ecg_buffer.extend(new_samples)

        # 2) redraw ECG
        ydata = np.fromiter(self.ecg_buffer, dtype=float,
                            count=len(self.ecg_buffer))
        self.ecg_line.set_ydata(ydata)
        self.canvas.draw_idle()

        # 3) HR card
        hr = self.sim.hr_actual
        self.hr_history.append(hr)
        avg = sum(self.hr_history) / len(self.hr_history)
        hr_text, hr_color = hr_status(hr)
        self.card_hr["value"].config(text=f"{hr:0.0f}", fg=hr_color)
        self.card_hr["sub"].config(
            text=f"{hr_text}  ·  avg {avg:0.0f}", fg=hr_color)

        # 4) SpO2 card
        sp = self.sim.spo2
        sp_text, sp_color = spo2_status(sp)
        self.card_spo2["value"].config(text=f"{sp:0.1f}", fg=sp_color)
        self.card_spo2["sub"].config(text=sp_text, fg=sp_color)

        # 5) Temp card
        t = self.sim.temp
        t_text, t_color = temp_status(t)
        self.card_temp["value"].config(text=f"{t:0.1f}", fg=t_color)
        self.card_temp["sub"].config(text=t_text, fg=t_color)

        # 6) Distance card (with derived RSSI)
        d = self.sim.distance_m
        d_text, d_color = distance_status(d)
        rssi = rssi_from_distance(d)
        self.card_dist["value"].config(text=f"{d:0.1f}", fg=d_color)
        self.card_dist["sub"].config(
            text=f"{d_text}  ·  RSSI {rssi:0.0f} dBm", fg=d_color)

        # 7) Clock
        self.clock_label.config(
            text=datetime.now().strftime("[ %H:%M:%S ]"))

        self.root.after(TICK_MS, self._update)

    def run(self):
        self._update()
        self.root.mainloop()


if __name__ == "__main__":
    DemoUI().run()
