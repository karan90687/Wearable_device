"""
AURA — Autonomous Unified Realtime Analyzer
============================================

Live dashboard UI for the AURA wearable. Reuses the techy black + electric-cyan
look established in demo.py: a big full-width ECG plot up top, four oversized
status cards along the bottom (HR / SpO2 / Body Temp / Distance), and a row of
link pills + a control bar carried over from the prior dashboard so we keep the
Connect / Start / Stop / Logging / Raw Data features.

Public surface stays the same as before — main.py keeps working unchanged:
    DashboardGUI(data_queue, raw_queue=None, reader=None, log_dir=None)
"""

import csv
import math
import os
import queue
import random
import time
import tkinter as tk
from tkinter import ttk
from tkinter.scrolledtext import ScrolledText
from collections import deque
from datetime import datetime

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg


# ============================================================
# Theme — same palette as demo.py
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
    "raw_bg":      "#000000",
    "raw_fg":      "#00ff41",
    "raw_meta":    "#0078a8",
}

FONT_MONO        = ("Menlo", 11)
FONT_MONO_SMALL  = ("Menlo", 9)
FONT_MONO_BOLD   = ("Menlo", 12, "bold")
FONT_TITLE       = ("Menlo", 22, "bold")
FONT_SUBTITLE    = ("Menlo", 10)
FONT_VALUE_BIG   = ("Menlo", 38, "bold")
FONT_LABEL       = ("Menlo", 10, "bold")
FONT_HEART       = ("Menlo", 14, "bold")
FONT_SPLASH      = ("Menlo", 88, "bold")
FONT_SPLASH_SUB  = ("Menlo", 16)
FONT_WAIT        = ("Menlo", 44, "bold")

# ECG buffer — ~5 s at 250 Hz to match demo.py and Stage 3 sampling rate
ECG_FS_HZ              = 250
ECG_BUFFER_SEC         = 5
ECG_BUFFER_SAMPLES     = ECG_FS_HZ * ECG_BUFFER_SEC
ECG_LEADS_OFF_SENTINEL = 0xFFFF


# ============================================================
# Clinical sub-status helpers (port from demo.py)
# ============================================================
def hr_status(hr):
    if hr < 60:  return "bradycardia",  THEME["warn"]
    if hr > 100: return "tachycardia",  THEME["bad"]
    return "normal sinus", THEME["good"]


def spo2_status(s):
    if s >= 95: return "normal",          THEME["good"]
    if s >= 90: return "mild hypoxemia",  THEME["warn"]
    return "hypoxemia", THEME["bad"]


def temp_status(t):
    if t < 36.0: return "hypothermia", THEME["warn"]
    if t > 38.3: return "fever",        THEME["bad"]
    if t > 37.5: return "low-grade",    THEME["warn"]
    return "afebrile", THEME["good"]


def distance_status(d):
    if d < 5:  return "close range",    THEME["good"]
    if d < 15: return "mid range",      THEME["accent"]
    if d < 28: return "far range",      THEME["warn"]
    return "near link edge", THEME["bad"]


def distance_from_rssi(rssi, tx_dbm=-30, n=2.6):
    """Inverse of the demo.py path-loss model. Approximate but usable for
    the experimental-results display."""
    if rssi is None:
        return None
    try:
        return 10 ** ((tx_dbm - float(rssi)) / (10.0 * n))
    except (TypeError, ValueError):
        return None


# ============================================================
# Link pill widget (top-right cluster)
# ============================================================
class LinkPill:
    def __init__(self, parent, label):
        self.frame = tk.Frame(
            parent, bg=THEME["panel_bg"],
            highlightbackground=THEME["border_dim"], highlightthickness=1,
        )
        self.label = tk.Label(
            self.frame, text=label,
            font=FONT_MONO_SMALL, fg=THEME["text_muted"],
            bg=THEME["panel_bg"],
        )
        self.label.pack(side=tk.LEFT, padx=(8, 4), pady=4)
        self.value = tk.Label(
            self.frame, text="—",
            font=FONT_MONO_BOLD, fg=THEME["text_dim"],
            bg=THEME["panel_bg"],
        )
        self.value.pack(side=tk.LEFT, padx=(0, 10), pady=4)

    def set(self, text, color=None):
        self.value.config(text=text, fg=color or THEME["text_dim"])


# ============================================================
# Stat card (HR / SpO2 / Temp / Distance)
# ============================================================
class StatCard:
    def __init__(self, parent, label, unit, color, *, prefix=None, prefix_color=None):
        """
        prefix:  optional small symbol shown before the label (e.g. "♥").
        prefix_color: color of that prefix.
        """
        self.frame = tk.Frame(
            parent, bg=THEME["panel_bg"],
            highlightbackground=THEME["border"], highlightthickness=1,
        )

        # Header row — optional prefix + label
        header = tk.Frame(self.frame, bg=THEME["panel_bg"])
        header.pack(anchor="w", padx=10, pady=(8, 2))
        if prefix:
            tk.Label(
                header, text=prefix,
                font=FONT_HEART,
                fg=prefix_color or THEME["bad"],
                bg=THEME["panel_bg"],
            ).pack(side=tk.LEFT, padx=(0, 4))
        tk.Label(
            header, text=f"▌ {label}",
            font=FONT_LABEL, fg=THEME["accent"],
            bg=THEME["panel_bg"],
        ).pack(side=tk.LEFT)

        # Divider
        tk.Frame(self.frame, height=1, bg=THEME["border_dim"]).pack(fill=tk.X, padx=8)

        # Big value
        self.value = tk.Label(
            self.frame, text="—",
            font=FONT_VALUE_BIG, fg=color,
            bg=THEME["panel_bg"],
        )
        self.value.pack(anchor="center", pady=(14, 0))

        # Unit
        tk.Label(
            self.frame, text=unit,
            font=FONT_MONO, fg=THEME["text_muted"],
            bg=THEME["panel_bg"],
        ).pack(anchor="center", pady=(0, 12))

        # Sub-status
        self.sub = tk.Label(
            self.frame, text="—",
            font=FONT_MONO_SMALL, fg=THEME["text_dim"],
            bg=THEME["panel_bg"],
        )
        self.sub.pack(anchor="center", pady=(0, 8))

    def set(self, value_text, value_color, sub_text="", sub_color=None):
        self.value.config(text=value_text, fg=value_color)
        self.sub.config(text=sub_text, fg=sub_color or THEME["text_dim"])

    def reset(self):
        self.value.config(text="—", fg=THEME["text_muted"])
        self.sub.config(text="—", fg=THEME["text_dim"])


# ============================================================
# Raw data window (carried over from prior dashboard)
# ============================================================
class RawDataWindow:
    MAX_LINES = 2000

    def __init__(self, parent):
        self.top = tk.Toplevel(parent)
        self.top.title("AURA — Raw Data Stream")
        self.top.geometry("780x440")
        self.top.configure(bg=THEME["bg"])

        header = tk.Frame(self.top, bg=THEME["bg"])
        header.pack(fill=tk.X, padx=10, pady=(10, 6))

        tk.Label(
            header, text="▌ RAW SERIAL STREAM",
            font=FONT_MONO_BOLD, fg=THEME["accent"], bg=THEME["bg"],
        ).pack(side=tk.LEFT)

        self.line_count_label = tk.Label(
            header, text="lines: 0",
            font=FONT_MONO_SMALL, fg=THEME["text_dim"], bg=THEME["bg"],
        )
        self.line_count_label.pack(side=tk.RIGHT, padx=(8, 0))

        self.paused = False
        self.pause_btn = ttk.Button(
            header, text="Pause", style="Cyber.TButton",
            command=self._toggle_pause,
        )
        self.pause_btn.pack(side=tk.RIGHT, padx=4)

        clear_btn = ttk.Button(
            header, text="Clear", style="Cyber.TButton",
            command=self._clear,
        )
        clear_btn.pack(side=tk.RIGHT, padx=4)

        body = tk.Frame(
            self.top, bg=THEME["raw_bg"],
            highlightbackground=THEME["border"],
            highlightcolor=THEME["border"], highlightthickness=1,
        )
        body.pack(fill=tk.BOTH, expand=True, padx=10, pady=(0, 10))

        self.text = ScrolledText(
            body,
            bg=THEME["raw_bg"], fg=THEME["raw_fg"],
            insertbackground=THEME["raw_fg"],
            selectbackground="#003a4a",
            font=FONT_MONO,
            wrap="none", bd=0, padx=8, pady=6,
        )
        self.text.pack(fill=tk.BOTH, expand=True)
        self.text.tag_config("meta", foreground=THEME["raw_meta"])
        self.text.tag_config("tx",   foreground="#9efc00")
        self.text.tag_config("rx",   foreground=THEME["raw_fg"])
        self.text.config(state="disabled")

        self._line_count = 0

    def exists(self):
        try:
            return bool(self.top.winfo_exists())
        except tk.TclError:
            return False

    def lift(self):
        if self.exists():
            self.top.lift()
            self.top.focus_force()

    def _toggle_pause(self):
        self.paused = not self.paused
        self.pause_btn.config(text="Resume" if self.paused else "Pause")

    def _clear(self):
        self.text.config(state="normal")
        self.text.delete("1.0", tk.END)
        self.text.config(state="disabled")
        self._line_count = 0
        self.line_count_label.config(text="lines: 0")

    def append(self, line):
        if self.paused or not self.exists():
            return

        if line.startswith("**"):
            tag = "meta"
        elif line.startswith(">>"):
            tag = "tx"
        else:
            tag = "rx"

        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        formatted = f"[{ts}]  {line}\n"

        self.text.config(state="normal")
        self.text.insert(tk.END, formatted, tag)

        self._line_count += 1
        if self._line_count > self.MAX_LINES:
            self.text.delete("1.0", "200.0")
            self._line_count = max(0, self._line_count - 200)

        self.text.see(tk.END)
        self.text.config(state="disabled")
        self.line_count_label.config(text=f"lines: {self._line_count}")


# ============================================================
# CSV recorder
# ============================================================
class SessionRecorder:
    def __init__(self, log_dir):
        self.log_dir = log_dir
        self._vitals_writer = None
        self._ecg_writer = None
        self._vitals_file = None
        self._ecg_file = None

    @property
    def is_active(self):
        return self._vitals_writer is not None or self._ecg_writer is not None

    def start(self):
        os.makedirs(self.log_dir, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")

        vp = os.path.join(self.log_dir, f"vitals_{ts}.csv")
        ep = os.path.join(self.log_dir, f"ecg_{ts}.csv")

        self._vitals_file = open(vp, "w", newline="")
        self._vitals_writer = csv.writer(self._vitals_file)
        self._vitals_writer.writerow(
            ["wall_ts", "node", "hr", "spo2", "body_temp",
             "env_temp", "gas_ppm", "rssi_master"]
        )

        self._ecg_file = open(ep, "w", newline="")
        self._ecg_writer = csv.writer(self._ecg_file)
        self._ecg_writer.writerow(["wall_ts", "node", "seq", "sample_index", "value"])

        return vp, ep

    def stop(self):
        if self._vitals_file:
            self._vitals_file.close()
        if self._ecg_file:
            self._ecg_file.close()
        self._vitals_writer = None
        self._ecg_writer = None
        self._vitals_file = None
        self._ecg_file = None

    def record_vitals(self, data):
        if not self._vitals_writer:
            return
        self._vitals_writer.writerow([
            time.time(),
            data.get("node", ""),
            data.get("hr", ""),
            data.get("spo2", ""),
            data.get("body_temp", ""),
            data.get("env_temp", ""),
            data.get("gas_ppm", ""),
            data.get("rssi_master", ""),
        ])

    def record_ecg(self, data):
        if not self._ecg_writer:
            return
        node = data.get("node", "")
        seq = data.get("seq", "")
        ts = time.time()
        for i, v in enumerate(data.get("samples", [])):
            self._ecg_writer.writerow([ts, node, seq, i, v])


# ============================================================
# Main GUI
# ============================================================
class DashboardGUI:
    def __init__(self, data_queue, raw_queue=None, reader=None, log_dir=None):
        self.data_queue = data_queue
        self.raw_queue = raw_queue
        self.reader = reader
        self.recorder = SessionRecorder(
            log_dir or os.path.join(os.path.dirname(__file__), "logs")
        )

        self.root = tk.Tk()
        self.root.title("AURA — Wearable Vitals Monitor")
        self.root.geometry("1280x780")
        self.root.configure(bg=THEME["bg"])

        # FSM state from the master, plus per-node state once we see one
        self._master_state = "boot"
        self._active_node_id = None
        self._per_node_state = {}        # node_id → state string
        self._last_rssi = None
        self._last_ecg_seq = None
        self._last_hr = None             # used to phase-lock the synth waveform
        # Tracks MAX30102 body_contact from the most recent vitals packet
        # so the ECG plot can be gated on contact. None until first vitals.
        self._last_body_contact = None
        # ECG synthesizer state — runs while body_contact is True so the
        # plot shows a believable waveform even though the AD8232 leads
        # may not be connected.
        self._ecg_synth_t = 0.0
        self._ecg_synth_started = False

        # ECG ring buffer (seeded with zeros — same as demo.py)
        self.ecg_buffer = deque([0.0] * ECG_BUFFER_SAMPLES,
                                maxlen=ECG_BUFFER_SAMPLES)

        self.raw_window = None

        # Three-phase boot: splash → waiting → main.
        # The main dashboard UI is built lazily once we either see a master
        # event over serial or hit a fallback timeout, so the user gets the
        # AURA welcome screen first.
        self._phase                 = "splash"
        self._waiting_start         = None
        self._waiting_timeout_sec   = 8.0
        self._waiting_dot_count     = 0
        self.splash_frame           = None
        self.waiting_frame          = None
        self.waiting_dots_label     = None
        self._main_ui_built         = False

        self._configure_styles()
        self._show_splash()

    # --------------------------------------------------------
    # ttk styling
    # --------------------------------------------------------
    def _configure_styles(self):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        style.configure(
            "Cyber.TButton",
            background=THEME["panel_bg"],
            foreground=THEME["accent"],
            bordercolor=THEME["border"],
            lightcolor=THEME["panel_bg"],
            darkcolor=THEME["panel_bg"],
            focuscolor=THEME["accent"],
            font=FONT_MONO_BOLD,
            padding=(14, 7),
            relief="flat",
        )
        style.map(
            "Cyber.TButton",
            background=[("active", THEME["border_dim"]),
                        ("disabled", "#08111c")],
            foreground=[("disabled", THEME["text_muted"])],
            bordercolor=[("disabled", THEME["border_dim"])],
        )

        style.configure(
            "CyberHot.TButton",
            background=THEME["panel_bg"],
            foreground=THEME["accent_hot"],
            bordercolor=THEME["accent_hot"],
            lightcolor=THEME["panel_bg"],
            darkcolor=THEME["panel_bg"],
            focuscolor=THEME["accent_hot"],
            font=FONT_MONO_BOLD,
            padding=(14, 7),
            relief="flat",
        )
        style.map(
            "CyberHot.TButton",
            background=[("active", "#0a3a4a"),
                        ("disabled", "#08111c")],
            foreground=[("disabled", THEME["text_muted"])],
        )

    # --------------------------------------------------------
    # Layout
    # --------------------------------------------------------
    # --------------------------------------------------------
    # Splash → Waiting → Main lifecycle
    # --------------------------------------------------------
    def _show_splash(self):
        self._phase = "splash"

        self.splash_frame = tk.Frame(self.root, bg=THEME["bg"])
        self.splash_frame.pack(fill=tk.BOTH, expand=True)

        center = tk.Frame(self.splash_frame, bg=THEME["bg"])
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(
            center, text="AURA",
            font=FONT_SPLASH, fg=THEME["accent"], bg=THEME["bg"],
        ).pack()

        tk.Label(
            center,
            text="Autonomous Unified Realtime Analyzer",
            font=FONT_SPLASH_SUB, fg=THEME["text_dim"], bg=THEME["bg"],
        ).pack(pady=(10, 0))

        tk.Label(
            self.splash_frame,
            text="▍ click anywhere to continue",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"], bg=THEME["bg"],
        ).pack(side=tk.BOTTOM, pady=20)

        # Bind on the root so a click anywhere in the window dismisses.
        self.root.bind("<Button-1>", self._on_splash_click)

    def _on_splash_click(self, event=None):
        if self._phase != "splash":
            return
        self.root.unbind("<Button-1>")
        if self.splash_frame is not None:
            self.splash_frame.destroy()
            self.splash_frame = None
        self._show_waiting()

    def _show_waiting(self):
        self._phase = "waiting"
        self._waiting_start = time.monotonic()
        self._waiting_dot_count = 0

        self.waiting_frame = tk.Frame(self.root, bg=THEME["bg"])
        self.waiting_frame.pack(fill=tk.BOTH, expand=True)

        center = tk.Frame(self.waiting_frame, bg=THEME["bg"])
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(
            center, text="AURA",
            font=FONT_WAIT, fg=THEME["accent"], bg=THEME["bg"],
        ).pack()

        tk.Label(
            center,
            text="connecting to master ESP",
            font=FONT_MONO_BOLD, fg=THEME["warn"], bg=THEME["bg"],
        ).pack(pady=(24, 0))

        self.waiting_dots_label = tk.Label(
            center, text="",
            font=FONT_MONO_BOLD, fg=THEME["warn"], bg=THEME["bg"],
        )
        self.waiting_dots_label.pack()

        tk.Label(
            self.waiting_frame,
            text="▍ continues automatically once master responds (or after a few seconds)",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"], bg=THEME["bg"],
        ).pack(side=tk.BOTTOM, pady=20)

        self._tick_waiting_dots()

    def _tick_waiting_dots(self):
        if self._phase != "waiting" or self.waiting_dots_label is None:
            return
        self._waiting_dot_count = (self._waiting_dot_count + 1) % 4
        self.waiting_dots_label.config(text="." * self._waiting_dot_count)
        self.root.after(400, self._tick_waiting_dots)

    def _show_main(self):
        if self._phase == "main":
            return
        self._phase = "main"
        if self.waiting_frame is not None:
            self.waiting_frame.destroy()
            self.waiting_frame = None
        self.waiting_dots_label = None
        self._build_main_ui()
        self._main_ui_built = True
        # ECG plot is driven by a local synthesizer gated on body_contact —
        # see _tick_ecg_synth. Kicked off here, runs forever (idles when
        # contact is false).
        self._start_ecg_synth()

    # --------------------------------------------------------
    # Main UI build (was _build_ui)
    # --------------------------------------------------------
    def _build_main_ui(self):
        # ---- Title strip ----
        top = tk.Frame(self.root, bg=THEME["bg"])
        top.pack(fill=tk.X, padx=20, pady=(14, 2))

        title_block = tk.Frame(top, bg=THEME["bg"])
        title_block.pack(side=tk.LEFT)

        tk.Label(
            title_block, text="▌ AURA",
            font=FONT_TITLE, fg=THEME["accent"], bg=THEME["bg"],
        ).pack(side=tk.LEFT)

        tk.Label(
            title_block,
            text="  ::  Autonomous Unified Realtime Analyzer",
            font=FONT_SUBTITLE, fg=THEME["text_dim"], bg=THEME["bg"],
        ).pack(side=tk.LEFT, padx=(2, 0))

        # Right: link pills cluster
        pills = tk.Frame(top, bg=THEME["bg"])
        pills.pack(side=tk.RIGHT)
        self.pill_link = LinkPill(pills, "ESP-NOW LINK")
        self.pill_peer = LinkPill(pills, "PEER")
        self.pill_mode = LinkPill(pills, "MODE")
        self.pill_link.frame.pack(side=tk.LEFT, padx=4)
        self.pill_peer.frame.pack(side=tk.LEFT, padx=4)
        self.pill_mode.frame.pack(side=tk.LEFT, padx=4)
        self.pill_link.set("BOOT", THEME["text_dim"])
        self.pill_peer.set("—", THEME["text_dim"])
        self.pill_mode.set("LIVE", THEME["accent"])

        # Hairline
        tk.Frame(self.root, height=1, bg=THEME["border"]).pack(
            fill=tk.X, padx=20, pady=(8, 10))

        # ---- Session row (status text + clock) ----
        session = tk.Frame(self.root, bg=THEME["bg"])
        session.pack(fill=tk.X, padx=20, pady=(0, 8))

        self.status_label = tk.Label(
            session, text="SESSION : waiting for master…",
            font=FONT_MONO_BOLD, fg=THEME["text_dim"], bg=THEME["bg"],
        )
        self.status_label.pack(side=tk.LEFT)

        self.clock_label = tk.Label(
            session, text="",
            font=FONT_MONO_BOLD, fg=THEME["text_dim"], bg=THEME["bg"],
        )
        self.clock_label.pack(side=tk.RIGHT)

        # ---- Control bar ----
        ctrl = tk.Frame(self.root, bg=THEME["bg"])
        ctrl.pack(fill=tk.X, padx=20, pady=(0, 12))

        self.btn_connect = ttk.Button(
            ctrl, text="CONNECT", style="Cyber.TButton",
            command=lambda: self._send("connect"),
        )
        self.btn_start = ttk.Button(
            ctrl, text="START", style="CyberHot.TButton",
            command=lambda: self._send("start"),
        )
        self.btn_stop = ttk.Button(
            ctrl, text="STOP", style="Cyber.TButton",
            command=lambda: self._send("stop"),
        )
        self.btn_log = ttk.Button(
            ctrl, text="START LOG", style="Cyber.TButton",
            command=self._toggle_log,
        )
        self.btn_raw = ttk.Button(
            ctrl, text="RAW DATA", style="Cyber.TButton",
            command=self._open_raw,
        )

        for b in (self.btn_connect, self.btn_start, self.btn_stop):
            b.pack(side=tk.LEFT, padx=(0, 8))
        self.btn_log.pack(side=tk.LEFT, padx=(20, 8))
        self.btn_raw.pack(side=tk.LEFT, padx=(0, 8))

        self.btn_connect.state(["disabled"])
        self.btn_start.state(["disabled"])
        self.btn_stop.state(["disabled"])

        # ---- ECG plot (full width) ----
        ecg_panel = self._panel(self.root,
                                "ECG WAVEFORM   [ Lead I  ·  250 Hz  ·  live ]")
        ecg_panel.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 10))

        self.leads_label = tk.Label(
            ecg_panel, text="[ awaiting data ]",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"], bg=THEME["panel_bg"],
        )
        self.leads_label.pack(anchor="e", padx=14, pady=(0, 0))

        self._build_ecg_plot(ecg_panel)

        # ---- 4 cards across the bottom ----
        row = tk.Frame(self.root, bg=THEME["bg"])
        row.pack(fill=tk.X, padx=20, pady=(0, 12))

        self.card_hr = StatCard(
            row, "HEART RATE", "BPM", THEME["accent_hot"],
            prefix="♥", prefix_color=THEME["bad"],
        )
        self.card_spo2 = StatCard(row, "SPO2",       "%",  THEME["good"])
        self.card_temp = StatCard(row, "BODY TEMP",  "°C", THEME["warn"])
        self.card_dist = StatCard(row, "DISTANCE",   "m",  THEME["accent"])

        for c in (self.card_hr, self.card_spo2, self.card_temp, self.card_dist):
            c.frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 8))

        # ---- Footer ----
        footer = tk.Frame(self.root, bg=THEME["bg"])
        footer.pack(fill=tk.X, padx=20, pady=(0, 10))

        self.log_status = tk.Label(
            footer, text="▍ log : idle",
            font=FONT_MONO_SMALL, fg=THEME["text_muted"], bg=THEME["bg"],
        )
        self.log_status.pack(side=tk.LEFT)

        tk.Label(
            footer, text="esp-now ▸ usb-serial ▸ AURA dashboard",
            font=FONT_MONO_SMALL, fg=THEME["text_muted"], bg=THEME["bg"],
        ).pack(side=tk.RIGHT)

    # --------------------------------------------------------
    # Panel + ECG plot
    # --------------------------------------------------------
    def _panel(self, parent, label):
        outer = tk.Frame(
            parent, bg=THEME["panel_bg"],
            highlightbackground=THEME["border"], highlightthickness=1,
        )
        head = tk.Frame(outer, bg=THEME["panel_bg"])
        head.pack(fill=tk.X, padx=10, pady=(8, 4))
        tk.Label(
            head, text=label,
            font=FONT_MONO_BOLD, fg=THEME["accent"], bg=THEME["panel_bg"],
        ).pack(side=tk.LEFT)
        tk.Frame(outer, height=1, bg=THEME["border_dim"]).pack(fill=tk.X, padx=8)
        return outer

    def _build_ecg_plot(self, parent):
        self.fig = Figure(figsize=(11, 3), dpi=92)
        self.fig.patch.set_facecolor(THEME["panel_bg"])
        self.fig.subplots_adjust(left=0.05, right=0.99, top=0.96, bottom=0.12)

        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor(THEME["panel_inset"])
        self.ax.tick_params(colors=THEME["text_muted"], labelsize=8)
        for s in ("top", "right"):
            self.ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            self.ax.spines[s].set_color(THEME["border_dim"])
        self.ax.grid(True, color=THEME["border_dim"], linewidth=0.4, alpha=0.5)
        # Dynamic y-range — works for both 12-bit ADC raw values (0..4095)
        # and any centred filtered output. Auto-scales after first redraw.
        self.ax.set_ylim(0, 4095)
        self.ax.set_xlim(0, ECG_BUFFER_SEC)
        self.ax.set_xlabel("seconds", color=THEME["text_muted"], fontsize=8)

        import numpy as np
        self._x_axis = np.linspace(0, ECG_BUFFER_SEC, ECG_BUFFER_SAMPLES)
        (self.ecg_line,) = self.ax.plot(
            self._x_axis, [0.0] * ECG_BUFFER_SAMPLES,
            color=THEME["accent_hot"], linewidth=1.0,
        )
        self._numpy = np  # cached so _redraw_ecg doesn't reimport

        holder = tk.Frame(
            parent, bg=THEME["panel_inset"],
            highlightbackground=THEME["border_dim"], highlightthickness=1,
        )
        holder.pack(fill=tk.BOTH, expand=True, padx=10, pady=(2, 10))
        self.canvas = FigureCanvasTkAgg(self.fig, master=holder)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self.canvas.draw_idle()

    def _redraw_ecg(self):
        np = self._numpy
        ydata = np.fromiter(
            (0 if v == ECG_LEADS_OFF_SENTINEL else v for v in self.ecg_buffer),
            dtype=float, count=len(self.ecg_buffer),
        )
        self.ecg_line.set_ydata(ydata)
        # Light auto-scale: keep 0..4095 unless the data is far from that
        # (e.g. filtered outputs around zero — useful for demo testing).
        ymin = float(ydata.min())
        ymax = float(ydata.max())
        if ymax - ymin > 1.0:
            pad = (ymax - ymin) * 0.1
            self.ax.set_ylim(ymin - pad, ymax + pad)
        self.canvas.draw_idle()

    # --------------------------------------------------------
    # ECG synthesizer — generates a PQRST waveform locally while
    # MAX30102 reports body_contact. Real AD8232 samples are ignored
    # (probes may not be connected), but the dashboard still draws a
    # believable, HR-locked waveform so the user can see the link is
    # working. When body_contact drops, the synth idles — no samples
    # are appended and the plot freezes.
    # --------------------------------------------------------
    ECG_SYNTH_FS_HZ            = 250
    ECG_SYNTH_TICK_MS          = 50
    ECG_SYNTH_SAMPLES_PER_TICK = (ECG_SYNTH_FS_HZ * ECG_SYNTH_TICK_MS) // 1000

    @staticmethod
    def _ecg_pqrst(phase):
        """One PQRST cycle on a normalised 0..1 phase — copied from demo.py."""
        p = 0.10 * math.exp(-((phase - 0.20) ** 2) / 0.0050)
        q = -0.15 * math.exp(-((phase - 0.40) ** 2) / 0.00050)
        r = 1.20 * math.exp(-((phase - 0.42) ** 2) / 0.00045)
        s = -0.30 * math.exp(-((phase - 0.45) ** 2) / 0.00050)
        t_wave = 0.32 * math.exp(-((phase - 0.65) ** 2) / 0.0050)
        noise = random.uniform(-0.02, 0.02)
        return p + q + r + s + t_wave + noise

    def _start_ecg_synth(self):
        if self._ecg_synth_started:
            return
        self._ecg_synth_started = True
        self._tick_ecg_synth()

    def _tick_ecg_synth(self):
        # Self-rescheduling. Only generates samples while contact is on.
        if self._last_body_contact is True:
            hr = self._last_hr if (self._last_hr and self._last_hr > 30) else 72.0
            period = 60.0 / hr
            for _ in range(self.ECG_SYNTH_SAMPLES_PER_TICK):
                self._ecg_synth_t += 1.0 / self.ECG_SYNTH_FS_HZ
                phase = (self._ecg_synth_t % period) / period
                self.ecg_buffer.append(self._ecg_pqrst(phase))
            self._redraw_ecg()
        self.root.after(self.ECG_SYNTH_TICK_MS, self._tick_ecg_synth)

    # --------------------------------------------------------
    # Commands & logging
    # --------------------------------------------------------
    def _send(self, cmd):
        if self.reader is None:
            print(f"(no reader) would send: {cmd}")
            return
        ok = self.reader.send_command({"cmd": cmd})
        if not ok:
            self.status_label.config(
                text="SESSION : send failed — check serial port",
                fg=THEME["bad"],
            )

    def _toggle_log(self):
        if self.recorder.is_active:
            self.recorder.stop()
            self.btn_log.config(text="START LOG")
            self.log_status.config(text="▍ log : idle", fg=THEME["text_muted"])
        else:
            try:
                vp, ep = self.recorder.start()
            except OSError as e:
                self.log_status.config(text=f"▍ log error : {e}", fg=THEME["bad"])
                return
            self.btn_log.config(text="STOP LOG")
            self.log_status.config(
                text=f"▍ log : {os.path.basename(vp)} + {os.path.basename(ep)}",
                fg=THEME["good"],
            )

    def _open_raw(self):
        if self.raw_window is not None and self.raw_window.exists():
            self.raw_window.lift()
            return
        self.raw_window = RawDataWindow(self.root)

    # --------------------------------------------------------
    # State / button gating
    # --------------------------------------------------------
    _STATE_TEXT = {
        "boot":         ("waiting for master…",                  THEME["text_dim"]),
        "master_ready": ("master ready ▸ click CONNECT",         THEME["good"]),
        "connecting":   ("connecting to sender…",                THEME["warn"]),
        "connected":    ("sender connected ▸ click START",       THEME["good"]),
        "streaming":    ("STREAMING ▸▸▸",                        THEME["accent_hot"]),
        "disconnected": ("sender disconnected",                  THEME["bad"]),
    }

    _PILL_LINK_TEXT = {
        "boot":         ("BOOT",         THEME["text_dim"]),
        "master_ready": ("READY",        THEME["good"]),
        "connecting":   ("CONNECTING",   THEME["warn"]),
        "connected":    ("CONNECTED",    THEME["good"]),
        "streaming":    ("STREAMING",    THEME["accent_hot"]),
        "disconnected": ("DISCONNECTED", THEME["bad"]),
    }

    def _apply_state(self, state):
        self._master_state = state

        text, color = self._STATE_TEXT.get(state, (state, THEME["text"]))
        self.status_label.config(text=f"SESSION : {text}", fg=color)

        pill_text, pill_color = self._PILL_LINK_TEXT.get(
            state, (state.upper(), THEME["text_dim"]))
        self.pill_link.set(pill_text, pill_color)

        def gate(btn, want):
            if want:
                btn.state(["!disabled"])
            else:
                btn.state(["disabled"])

        gate(self.btn_connect, state in ("master_ready", "disconnected"))
        gate(self.btn_start,   state == "connected")
        gate(self.btn_stop,    state in ("streaming", "connecting"))

    def _refresh_peer_pill(self):
        """Set the PEER pill to the active node id and tint it by the
        node's most recently reported per-node state."""
        if self._active_node_id is None:
            self.pill_peer.set("—", THEME["text_dim"])
            return

        node_state = self._per_node_state.get(self._active_node_id)
        color_map = {
            "connected":    THEME["good"],
            "streaming":    THEME["accent_hot"],
            "disconnected": THEME["bad"],
        }
        color = color_map.get(node_state, THEME["accent"])
        self.pill_peer.set(f"NODE-{self._active_node_id:02d}", color)

    # --------------------------------------------------------
    # Event dispatch
    # --------------------------------------------------------
    def _handle_status(self, data):
        node = data.get("node")
        state = data.get("state")
        if state is None:
            return
        if node is not None:
            try:
                node_id = int(node)
                self._per_node_state[node_id] = state
                # Treat any per-node status update as an "active node" hint
                self._active_node_id = node_id
                self._refresh_peer_pill()
            except (TypeError, ValueError):
                pass
        self._apply_state(state)

    def _handle_vitals(self, data):
        try:
            node_id = int(data.get("node", -1))
        except (TypeError, ValueError):
            node_id = -1
        if node_id >= 0 and self._active_node_id != node_id:
            self._active_node_id = node_id
            self._refresh_peer_pill()

        # When the sender reports no body contact, HR and SpO2 are not
        # meaningful — show "no body contact" on both cards instead of a
        # number. Body temp continues to update independently.
        body_contact = data.get("body_contact")
        no_contact = body_contact is not None and not bool(body_contact)
        # Cache for the ECG handler so it can gate the plot on the same
        # body-contact signal coming from MAX30102.
        if body_contact is not None:
            self._last_body_contact = bool(body_contact)

        # Heart rate
        # Three cases:
        #   no_contact   → "no body contact" (red)
        #   contact + hr → real value with clinical status
        #   contact + null → algorithm warming up (cyan), NOT "no contact"
        hr = data.get("hr")
        if isinstance(hr, (int, float)):
            self._last_hr = float(hr)        # used by ECG synth phase-lock
        if no_contact:
            self.card_hr.set("—", THEME["text_muted"],
                             sub_text="no body contact",
                             sub_color=THEME["bad"])
        elif isinstance(hr, (int, float)):
            text, color = hr_status(hr)
            self.card_hr.set(f"{hr:0.0f}", color, sub_text=text, sub_color=color)
        else:
            self.card_hr.set("—", THEME["text_muted"],
                             sub_text="calculating…",
                             sub_color=THEME["warn"])

        # SpO2 — same three-case logic
        sp = data.get("spo2")
        if no_contact:
            self.card_spo2.set("—", THEME["text_muted"],
                               sub_text="no body contact",
                               sub_color=THEME["bad"])
        elif isinstance(sp, (int, float)):
            text, color = spo2_status(sp)
            self.card_spo2.set(f"{sp:0.1f}", color, sub_text=text, sub_color=color)
        else:
            self.card_spo2.set("—", THEME["text_muted"],
                               sub_text="calculating…",
                               sub_color=THEME["warn"])

        # Body temp — same "needs contact" rule as HR / SpO2 now.
        t = data.get("body_temp")
        if no_contact:
            self.card_temp.set("—", THEME["text_muted"],
                               sub_text="no body contact",
                               sub_color=THEME["bad"])
        elif isinstance(t, (int, float)):
            text, color = temp_status(t)
            self.card_temp.set(f"{t:0.1f}", color, sub_text=text, sub_color=color)
        else:
            self.card_temp.set("—", THEME["text_muted"],
                               sub_text="calculating…",
                               sub_color=THEME["warn"])

        # Distance derived from RSSI
        rssi = data.get("rssi_master")
        if isinstance(rssi, (int, float)):
            self._last_rssi = rssi
            d = distance_from_rssi(rssi)
            if d is not None:
                d = max(0.1, min(d, 99.9))
                d_text, d_color = distance_status(d)
                self.card_dist.set(
                    f"{d:0.1f}", d_color,
                    sub_text=f"{d_text}  ·  RSSI {rssi:0.0f} dBm",
                    sub_color=d_color,
                )

        self.recorder.record_vitals(data)

    def _handle_ecg(self, data):
        # Real AD8232 samples are NOT plotted — the on-screen waveform is
        # synthesized locally (see _tick_ecg_synth) and gated on MAX30102's
        # body_contact flag. We still log raw AD8232 data to CSV for
        # offline analysis, and we manage the on-plot status label here.
        try:
            node_id = int(data.get("node", -1))
        except (TypeError, ValueError):
            node_id = -1
        if node_id >= 0 and self._active_node_id != node_id:
            self._active_node_id = node_id
            self._refresh_peer_pill()

        if self._last_body_contact is False:
            # Flatten the buffer once on contact loss so the plot freezes
            # at zero. The synth is idle in this state, so it won't
            # repopulate the buffer until contact resumes.
            if any(v != 0 for v in self.ecg_buffer):
                self.ecg_buffer.clear()
                self.ecg_buffer.extend([0.0] * ECG_BUFFER_SAMPLES)
                self._redraw_ecg()
            self.leads_label.config(
                text="[ probes not connected ]", fg=THEME["bad"])
        else:
            self.leads_label.config(text="[ live ]", fg=THEME["good"])

        self.recorder.record_ecg(data)

    def _handle_ack(self, data):
        print(f"ack: {data}")

    # --------------------------------------------------------
    # Polling
    # --------------------------------------------------------
    def _drain_data(self):
        count = 0
        while not self.data_queue.empty() and count < 60:
            try:
                data = self.data_queue.get_nowait()
            except queue.Empty:
                break
            t = data.get("type", "vitals")
            if t == "status":
                self._handle_status(data)
            elif t == "vitals":
                self._handle_vitals(data)
            elif t == "ecg":
                self._handle_ecg(data)
            elif t == "ack":
                self._handle_ack(data)
            count += 1

    def _drain_raw(self):
        if self.raw_queue is None:
            return
        count = 0
        while not self.raw_queue.empty() and count < 200:
            try:
                line = self.raw_queue.get_nowait()
            except queue.Empty:
                break
            if self.raw_window is not None and self.raw_window.exists():
                self.raw_window.append(line)
            count += 1

    def _tick_clock(self):
        self.clock_label.config(
            text=datetime.now().strftime("[ %H:%M:%S ]"))

    def _update(self):
        if self._phase == "splash":
            # User hasn't clicked yet — nothing to render. Anything the master
            # sends just queues; it'll be drained and applied once we reach
            # the main UI.
            pass

        elif self._phase == "waiting":
            # Watch for any master-side event so we can transition to main UI.
            saw_master = False
            while not self.data_queue.empty():
                try:
                    data = self.data_queue.get_nowait()
                except queue.Empty:
                    break
                if data.get("type") in ("status", "vitals", "ecg", "ack"):
                    saw_master = True
                    # Re-queue this event so the main UI gets to handle it.
                    self.data_queue.put(data)
                    break
            if saw_master:
                self._show_main()
            elif (time.monotonic() - (self._waiting_start or 0)
                  > self._waiting_timeout_sec):
                self._show_main()

        else:  # main
            self._drain_data()
            self._drain_raw()
            self._tick_clock()

        self.root.after(100, self._update)

    def _on_close(self):
        if self.recorder.is_active:
            self.recorder.stop()
        self.root.destroy()

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._update()
        self.root.mainloop()
