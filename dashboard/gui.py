"""
Dashboard GUI — Stage 4 (techy theme)

Black background with electric-cyan accents and monospace typography.
Per-node panels render lazily as nodes appear in the data stream. Each
panel shows vitals, an ECG live plot, and a state badge.

A "Raw Data" button opens a separate window that tails every raw line
arriving over the serial port — green on black, terminal vibe.
"""

import csv
import os
import queue
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
# Theme
# ============================================================
THEME = {
    # Surfaces
    "bg":           "#04080f",   # near-black, hint of blue
    "panel_bg":     "#0a1525",   # surface
    "panel_inset":  "#050b15",   # darker inset for plots/text
    # Lines & borders
    "border":       "#00b8e6",   # electric cyan
    "border_dim":   "#0a3a55",
    # Text
    "text":         "#e6f6ff",
    "text_dim":     "#5b8aa8",
    "text_muted":   "#37607a",
    # Accents
    "accent":       "#00d9ff",
    "accent_hot":   "#00ffe1",
    # Status
    "good":         "#00ff9c",
    "warn":         "#ffaa1a",
    "bad":          "#ff3366",
    # Raw console
    "raw_bg":       "#000000",
    "raw_fg":       "#00ff41",   # matrix green
    "raw_meta":     "#0078a8",
}

# Fonts — Menlo on macOS; Tk falls back to a system mono elsewhere.
FONT_MONO       = ("Menlo", 10)
FONT_MONO_SMALL = ("Menlo", 9)
FONT_MONO_BOLD  = ("Menlo", 11, "bold")
FONT_TITLE      = ("Menlo", 20, "bold")
FONT_VALUE      = ("Menlo", 14, "bold")
FONT_BADGE      = ("Menlo", 10, "bold")

# ECG buffer: ~5 seconds at 250 Hz (Stage 3 default rate)
ECG_BUFFER_SAMPLES = 1250
ECG_LEADS_OFF_SENTINEL = 0xFFFF


# ============================================================
# Reusable styled widgets
# ============================================================
def cyber_frame(parent, **kwargs):
    """Frame with a cyan hairline border for the techy panel look."""
    return tk.Frame(
        parent,
        bg=kwargs.pop("bg", THEME["panel_bg"]),
        highlightbackground=THEME["border"],
        highlightcolor=THEME["border"],
        highlightthickness=1,
        bd=0,
        **kwargs,
    )


def cyber_label(parent, text, *, font=FONT_MONO, fg=None, bg=None, **kw):
    return tk.Label(
        parent, text=text, font=font,
        fg=fg or THEME["text"],
        bg=bg or THEME["panel_bg"],
        **kw,
    )


# ============================================================
# Per-node panel
# ============================================================
class NodePanel:
    BADGE_COLORS = {
        "connected":    THEME["good"],
        "streaming":    THEME["accent_hot"],
        "disconnected": THEME["bad"],
        "connecting":   THEME["warn"],
    }

    VITAL_FIELDS = [
        ("HEART RATE",     "hr",          "BPM"),
        ("SPO2",           "spo2",        "%"),
        ("BODY TEMP",      "body_temp",   "°C"),
        ("RSSI / MASTER",  "rssi_master", "dBm"),
    ]

    def __init__(self, parent, node_id):
        self.node_id = node_id

        self.frame = cyber_frame(parent)

        # Header strip
        header = tk.Frame(self.frame, bg=THEME["panel_bg"])
        header.pack(fill=tk.X, padx=10, pady=(8, 4))

        cyber_label(header, f"▌ NODE {node_id:02d}",
                    font=FONT_MONO_BOLD,
                    fg=THEME["accent"]).pack(side=tk.LEFT)

        self.badge = cyber_label(
            header, "● IDLE",
            font=FONT_BADGE, fg=THEME["text_dim"],
        )
        self.badge.pack(side=tk.RIGHT)

        # Divider
        tk.Frame(self.frame, height=1, bg=THEME["border_dim"]).pack(
            fill=tk.X, padx=8, pady=(2, 8))

        # Vitals grid
        vitals_frame = tk.Frame(self.frame, bg=THEME["panel_bg"])
        vitals_frame.pack(fill=tk.X, padx=10, pady=(0, 6))

        self.value_labels = {}
        for i, (label_text, key, unit) in enumerate(self.VITAL_FIELDS):
            row = tk.Frame(vitals_frame, bg=THEME["panel_bg"])
            row.pack(fill=tk.X, pady=2)

            cyber_label(row, label_text + " :",
                        font=FONT_MONO_SMALL,
                        fg=THEME["text_dim"],
                        width=18, anchor="w").pack(side=tk.LEFT)

            val = cyber_label(row, "––",
                              font=FONT_VALUE,
                              fg=THEME["accent_hot"],
                              width=8, anchor="e")
            val.pack(side=tk.LEFT)

            cyber_label(row, unit,
                        font=FONT_MONO_SMALL,
                        fg=THEME["text_muted"]).pack(side=tk.LEFT, padx=(6, 0))

            self.value_labels[key] = val

        # ECG section header + leads-off line
        ecg_header = tk.Frame(self.frame, bg=THEME["panel_bg"])
        ecg_header.pack(fill=tk.X, padx=10, pady=(8, 2))
        cyber_label(ecg_header, "ECG WAVEFORM",
                    font=FONT_MONO_BOLD,
                    fg=THEME["accent"]).pack(side=tk.LEFT)
        self.leads_label = cyber_label(
            ecg_header, "[ awaiting data ]",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"],
        )
        self.leads_label.pack(side=tk.RIGHT)

        # ECG plot
        self.ecg_buffer = deque(maxlen=ECG_BUFFER_SAMPLES)
        self.last_ecg_seq = None
        self._build_ecg_plot()

    def _build_ecg_plot(self):
        self.fig = Figure(figsize=(4.6, 1.7), dpi=92)
        self.fig.patch.set_facecolor(THEME["panel_bg"])
        self.fig.subplots_adjust(left=0.06, right=0.99, top=0.95, bottom=0.10)

        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor(THEME["panel_inset"])
        self.ax.tick_params(colors=THEME["text_muted"], labelsize=7)
        for side in ("top", "right"):
            self.ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            self.ax.spines[side].set_color(THEME["border_dim"])

        self.ax.grid(True, color=THEME["border_dim"], linewidth=0.4, alpha=0.5)
        self.ax.set_ylim(0, 4095)
        self.ax.set_xlim(0, ECG_BUFFER_SAMPLES)
        self.ax.set_xticks([])

        (self.ecg_line,) = self.ax.plot(
            [], [], color=THEME["accent_hot"], linewidth=1.0,
        )

        canvas_holder = tk.Frame(
            self.frame, bg=THEME["panel_inset"],
            highlightbackground=THEME["border_dim"],
            highlightthickness=1,
        )
        canvas_holder.pack(fill=tk.BOTH, expand=True, padx=10, pady=(0, 10))

        self.canvas = FigureCanvasTkAgg(self.fig, master=canvas_holder)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self.canvas.draw_idle()

    def _redraw_ecg(self):
        if not self.ecg_buffer:
            return
        ys = [(0 if v == ECG_LEADS_OFF_SENTINEL else v) for v in self.ecg_buffer]
        xs = list(range(len(ys)))
        self.ecg_line.set_data(xs, ys)
        self.ax.set_xlim(0, max(len(ys), ECG_BUFFER_SAMPLES))
        self.canvas.draw_idle()

    def update_vitals(self, data):
        for key, label in self.value_labels.items():
            value = data.get(key, "––")
            if isinstance(value, float):
                label.config(text=f"{value:.1f}")
            elif isinstance(value, int):
                label.config(text=str(value))
            else:
                label.config(text=str(value))

    def update_ecg(self, data):
        samples = data.get("samples", [])
        seq = data.get("seq")
        if self.last_ecg_seq is not None and seq is not None:
            gap = seq - self.last_ecg_seq - 1
            if 0 < gap < 8:
                self.ecg_buffer.extend([ECG_LEADS_OFF_SENTINEL] * gap * 32)
        self.last_ecg_seq = seq

        leads_off = any(s == ECG_LEADS_OFF_SENTINEL for s in samples)
        self.ecg_buffer.extend(samples)

        if leads_off:
            self.leads_label.config(text="[ LEADS OFF — reattach electrodes ]",
                                    fg=THEME["bad"])
        else:
            self.leads_label.config(text="[ live ]", fg=THEME["good"])

        self._redraw_ecg()

    def set_state(self, state):
        color = self.BADGE_COLORS.get(state, THEME["text_dim"])
        text = f"● {state.upper()}"
        self.badge.config(text=text, fg=color)


# ============================================================
# Raw data window
# ============================================================
class RawDataWindow:
    """Toplevel that tails every raw serial line in green-on-black."""

    MAX_LINES = 2000

    def __init__(self, parent):
        self.top = tk.Toplevel(parent)
        self.top.title("Raw Data Stream")
        self.top.geometry("780x440")
        self.top.configure(bg=THEME["bg"])

        # Header bar
        header = tk.Frame(self.top, bg=THEME["bg"])
        header.pack(fill=tk.X, padx=10, pady=(10, 6))

        cyber_label(header, "▌ RAW SERIAL STREAM",
                    font=FONT_MONO_BOLD,
                    fg=THEME["accent"], bg=THEME["bg"]).pack(side=tk.LEFT)

        self.line_count_label = cyber_label(
            header, "lines: 0",
            font=FONT_MONO_SMALL,
            fg=THEME["text_dim"], bg=THEME["bg"],
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

        # Console body
        body = tk.Frame(
            self.top, bg=THEME["raw_bg"],
            highlightbackground=THEME["border"],
            highlightcolor=THEME["border"],
            highlightthickness=1,
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
        # Tags for differently-colored line categories.
        self.text.tag_config("meta", foreground=THEME["raw_meta"])
        self.text.tag_config("tx",   foreground="#9efc00")
        self.text.tag_config("rx",   foreground=THEME["raw_fg"])
        self.text.config(state="disabled")

        self._line_count = 0

    # ---- Lifecycle helpers ----------------------------------------
    def exists(self):
        try:
            return bool(self.top.winfo_exists())
        except tk.TclError:
            return False

    def lift(self):
        if self.exists():
            self.top.lift()
            self.top.focus_force()

    # ---- Console ops ----------------------------------------------
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

        # Categorise for color
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
            # Trim oldest 200 lines so the widget stays snappy.
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
        self.root.title("WEARABLE • VITALS MONITOR")
        self.root.geometry("1180x740")
        self.root.configure(bg=THEME["bg"])

        self._configure_ttk_styles()

        self.panels = {}                 # node_id → NodePanel
        self.raw_window = None           # RawDataWindow or None
        self._master_state = "boot"

        self._build_ui()

    # ----- ttk styling ----------------------------------------------
    def _configure_ttk_styles(self):
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

    # ----- UI construction ------------------------------------------
    def _build_ui(self):
        # Title strip
        title_strip = tk.Frame(self.root, bg=THEME["bg"])
        title_strip.pack(fill=tk.X, padx=20, pady=(14, 6))

        cyber_label(
            title_strip, "▌ WEARABLE  ::  VITALS  MONITOR",
            font=FONT_TITLE,
            fg=THEME["accent"],
            bg=THEME["bg"],
        ).pack(side=tk.LEFT)

        self.clock_label = cyber_label(
            title_strip, "",
            font=FONT_MONO,
            fg=THEME["text_dim"],
            bg=THEME["bg"],
        )
        self.clock_label.pack(side=tk.RIGHT)

        # Hairline
        tk.Frame(self.root, height=1, bg=THEME["border"]).pack(
            fill=tk.X, padx=20, pady=(0, 12))

        # Status bar
        status_row = tk.Frame(self.root, bg=THEME["bg"])
        status_row.pack(fill=tk.X, padx=20, pady=(0, 10))

        cyber_label(status_row, "STATUS :",
                    font=FONT_MONO_BOLD,
                    fg=THEME["text_dim"],
                    bg=THEME["bg"]).pack(side=tk.LEFT)

        self.status_label = cyber_label(
            status_row, "waiting for master…",
            font=FONT_MONO_BOLD,
            fg=THEME["text_dim"],
            bg=THEME["bg"],
        )
        self.status_label.pack(side=tk.LEFT, padx=(8, 0))

        # Control bar
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

        # Initial states
        self.btn_connect.state(["disabled"])
        self.btn_start.state(["disabled"])
        self.btn_stop.state(["disabled"])

        # Panel container
        outer = cyber_frame(self.root, bg=THEME["bg"])
        outer.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 10))

        self.panel_container = tk.Frame(outer, bg=THEME["bg"])
        self.panel_container.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        self.placeholder = cyber_label(
            self.panel_container,
            "▍ no sender nodes seen yet — click CONNECT once master is ready",
            font=FONT_MONO,
            fg=THEME["text_muted"],
            bg=THEME["bg"],
        )
        self.placeholder.pack(pady=60)

        # Footer log status
        footer = tk.Frame(self.root, bg=THEME["bg"])
        footer.pack(fill=tk.X, padx=20, pady=(0, 10))

        self.log_status = cyber_label(
            footer, "▍ log : idle",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"],
            bg=THEME["bg"],
        )
        self.log_status.pack(side=tk.LEFT)

        cyber_label(
            footer, "esp-now ▸ usb-serial ▸ tk dashboard",
            font=FONT_MONO_SMALL,
            fg=THEME["text_muted"],
            bg=THEME["bg"],
        ).pack(side=tk.RIGHT)

    # ----- Panel management -----------------------------------------
    def _ensure_panel(self, node_id):
        try:
            node_id = int(node_id)
        except (TypeError, ValueError):
            return None
        if node_id in self.panels:
            return self.panels[node_id]

        if self.placeholder is not None:
            self.placeholder.destroy()
            self.placeholder = None

        panel = NodePanel(self.panel_container, node_id)
        panel.frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=8, pady=4)
        self.panels[node_id] = panel
        return panel

    # ----- Commands & logging ---------------------------------------
    def _send(self, cmd):
        if self.reader is None:
            print(f"(no reader) would send: {cmd}")
            return
        ok = self.reader.send_command({"cmd": cmd})
        if not ok:
            self.status_label.config(text="send failed — check serial port",
                                     fg=THEME["bad"])

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

    # ----- State (button gating) ------------------------------------
    def _apply_state(self, state):
        self._master_state = state
        text_map = {
            "boot":         ("waiting for master…",                THEME["text_dim"]),
            "master_ready": ("master ready ▸ click CONNECT",       THEME["good"]),
            "connecting":   ("connecting to sender…",              THEME["warn"]),
            "connected":    ("sender connected ▸ click START",     THEME["good"]),
            "streaming":    ("STREAMING ▸▸▸",                      THEME["accent_hot"]),
            "disconnected": ("sender disconnected",                THEME["bad"]),
        }
        text, color = text_map.get(state, (state, THEME["text"]))
        self.status_label.config(text=text, fg=color)

        def gate(btn, want):
            if want:
                btn.state(["!disabled"])
            else:
                btn.state(["disabled"])

        gate(self.btn_connect, state in ("master_ready", "disconnected"))
        gate(self.btn_start,   state == "connected")
        gate(self.btn_stop,    state in ("streaming", "connecting"))

    # ----- Event dispatch -------------------------------------------
    def _handle_status(self, data):
        node = data.get("node")
        state = data.get("state")
        if state is None:
            return
        if node is not None:
            panel = self._ensure_panel(node)
            if panel:
                panel.set_state(state)
        self._apply_state(state)

    def _handle_vitals(self, data):
        panel = self._ensure_panel(data.get("node"))
        if panel:
            panel.update_vitals(data)
        self.recorder.record_vitals(data)

    def _handle_ecg(self, data):
        panel = self._ensure_panel(data.get("node"))
        if panel:
            panel.update_ecg(data)
        self.recorder.record_ecg(data)

    def _handle_ack(self, data):
        # Visible in raw-data window already. Console echo for stage 1/2 debug.
        print(f"ack: {data}")

    # ----- Polling --------------------------------------------------
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
            text=datetime.now().strftime("[ %H:%M:%S ]")
        )

    def _update(self):
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
