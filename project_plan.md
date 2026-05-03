# Wearable Body Vitals Monitor — Implementation Plan

This document is the working spec for the prototype build. It supersedes
`project_plan.md` (which described the older 2-sender + RSSI-proximity scope).

---

## 1. Scope

Prototype with **1 sender ESP + 1 master ESP + Python dashboard on laptop**.
Architecture is designed so a second sender can be added later **without any
sender-firmware change** — only the master's peer table and the dashboard
panel rendering change.

### Hardware
- **Sender:** ESP32 DevKit V1 (38-pin classic ESP32)
- **Master:** ESP32 DevKit V1 (38-pin classic ESP32)
- **Sensors on sender:**
  - MAX30102 (I²C) — HR + SpO₂ raw PPG (Red + IR)
  - TMP117 (I²C) — body temperature
  - AD8232 (analog → ADC) — ECG
- **Indicators on sender:** onboard LED (GPIO 2) + buzzer
- **Indicators on master:** onboard LED (GPIO 2)
- **Laptop:** runs Python dashboard, connects to master over USB serial

### Pin map (DevKit V1)
| Function | GPIO |
|----------|------|
| I²C SDA (MAX30102 + TMP117) | 21 |
| I²C SCL (MAX30102 + TMP117) | 22 |
| AD8232 OUT (ADC) | 34 (ADC1_CH6, input-only) |
| AD8232 LO+ | 32 |
| AD8232 LO- | 33 |
| Buzzer | 5 |
| Onboard LED (sender + master) | 2 |

---

## 2. End-to-end flow

```
[Dashboard (laptop)]  --USB serial JSON-->  [Master ESP]  --ESP-NOW-->  [Sender ESP]
        ^                                          |                          |
        |       USB serial JSON (status+data)      |                          |
        +------------------------------------------+                          |
                                                                              v
                                                                       sensors + LED + buzzer
```

### User-visible flow
1. User runs the dashboard (`python main.py`).
2. User plugs the master ESP into the laptop. Master boots, initialises
   peripherals, lights its onboard LED, and emits
   `{"type":"status","state":"master_ready"}` over USB serial.
3. Dashboard shows **"Master Ready"** and enables the **Connect** button.
4. User clicks **Connect** → dashboard sends `{"cmd":"connect"}` to master.
5. Master sends a `HELLO` over ESP-NOW to the hardcoded sender MAC and waits
   for a `READY` reply (with retries / timeout).
6. Sender (which has been booted, sensors initialised, sitting in IDLE):
   - Receives `HELLO`, replies `READY`.
   - Turns on its onboard LED.
   - Beeps the buzzer once.
   - Transitions to CONNECTED state.
7. Master receives `READY` → emits
   `{"type":"status","node":1,"state":"connected"}`.
   Dashboard now enables the **Start** button.
8. User clicks **Start** → dashboard sends `{"cmd":"start"}` to master →
   master sends `CMD_START` to sender → sender enters STREAMING.
9. Sender continuously emits vitals + ECG packets over ESP-NOW; master
   forwards each as a JSON line (`"type":"vitals"` and `"type":"ecg"`) over
   USB serial; dashboard plots/displays.
10. User clicks **Stop** → reverse path → sender returns to CONNECTED, LED
    stays on, buzzer silent. Streaming halts.
11. If sender dies / goes out of range → heartbeat times out → master emits
    `{"type":"status","node":1,"state":"disconnected"}`. Dashboard reflects.

---

## 3. Command flow — UI to Master ESP

This was the explicit question. Walk-through of one click.

### Transport
- **Channel:** USB serial (the same serial the master already uses to print
  output). Master's UART0 goes over the USB-UART chip on the DevKit.
- **Encoding:** **line-delimited JSON** in both directions. One JSON object
  per line, terminated by `\n`. Easy to read with `pyserial` and easy to
  print with `printf` from ESP-IDF.
- **Baud rate:** 115200 (matches existing `serial_reader.py`).

### Direction: dashboard → master (commands)
- The dashboard's **Connect/Start/Stop** buttons each build a JSON string
  and write it to the same `pyserial.Serial` instance the reader thread
  owns (writes are thread-safe on `Serial`).
- Example payloads:
  ```json
  {"cmd":"connect"}
  {"cmd":"start"}
  {"cmd":"stop"}
  ```
- The master runs a dedicated **UART RX task** that:
  1. Reads bytes from `stdin` (UART0) until it sees `\n`.
  2. Parses the line as JSON using `cJSON` (already in ESP-IDF).
  3. Looks at `"cmd"` and pushes a `cmd_t` enum value into a FreeRTOS
     command queue.
- The master's **state machine task** consumes the queue and acts:
  - `connect` → send `HELLO` packet to sender via ESP-NOW.
  - `start`   → send `CMD_START` packet via ESP-NOW.
  - `stop`    → send `CMD_STOP` packet via ESP-NOW.

### Direction: master → dashboard (events + data)
- Every line the master prints is a JSON object with a `"type"` field, so
  the dashboard's reader can dispatch.
- Event types:
  - `{"type":"status","state":"master_ready"}` — emitted once on boot.
  - `{"type":"status","node":1,"state":"connected"}`
  - `{"type":"status","node":1,"state":"streaming"}`
  - `{"type":"status","node":1,"state":"disconnected"}`
  - `{"type":"ack","cmd":"connect","ok":true}` — optional command ack.
- Data types:
  - `{"type":"vitals","node":1,"hr_red":...,"hr_ir":...,"body_temp":...}`
  - `{"type":"ecg","node":1,"seq":N,"samples":[...]}` — batched ECG.

### Important UART implementation notes
- **Don't print logs that aren't JSON to stdout.** ESP-IDF's `ESP_LOGI`
  goes to UART0 by default and would corrupt the JSON stream the dashboard
  reads. Two safe options:
  1. Route ESP-IDF logging to UART1 (different pins) so UART0 is JSON-only.
  2. Or keep logs on UART0 but make the dashboard reader skip lines that
     don't start with `{` (current `serial_reader.py` already does this).
  We'll take **option 2** for simplicity in the prototype — the dashboard
  already tolerates non-JSON lines.
- Use `printf("%s\n", json)` from the master, never `puts` without `\n`.
- The UART RX task must read with `fgets`-style line buffering, not
  byte-by-byte polling.

---

## 4. Protocol additions

### New ESP-NOW packet types (`common/protocol/include/protocol.h`)
```c
#define PACKET_TYPE_SENSOR_DATA  0x01   // existing — repurposed as "vitals"
#define PACKET_TYPE_ECG          0x02   // batched ECG samples
#define PACKET_TYPE_HELLO        0x10   // master → sender: handshake req
#define PACKET_TYPE_READY        0x11   // sender → master: handshake ack
#define PACKET_TYPE_CMD_START    0x12   // master → sender
#define PACKET_TYPE_CMD_STOP     0x13   // master → sender
#define PACKET_TYPE_HEARTBEAT    0x14   // bidirectional liveness
```

### New packet structs
```c
typedef struct __attribute__((packed)) {
    uint8_t  packet_type;   // PACKET_TYPE_SENSOR_DATA
    uint8_t  node_id;
    float    body_temp;     // Celsius from TMP117
    uint32_t ir;            // latest filtered MAX30102 IR
    uint32_t red;           // latest filtered MAX30102 RED
} vitals_packet_t;          // ~14 bytes

#define ECG_SAMPLES_PER_PACKET 32
typedef struct __attribute__((packed)) {
    uint8_t  packet_type;   // PACKET_TYPE_ECG
    uint8_t  node_id;
    uint16_t seq;
    uint16_t samples[ECG_SAMPLES_PER_PACKET];  // 12-bit ADC, leads_off uses 0xFFFF sentinel
} ecg_packet_t;             // ~68 bytes

typedef struct __attribute__((packed)) {
    uint8_t packet_type;    // HELLO / READY / CMD_START / CMD_STOP / HEARTBEAT
    uint8_t node_id;
} ctrl_packet_t;
```

### Rate budget
- **ECG sampling:** 250 Hz (every 4 ms).
- **ECG packet rate:** 32 samples per packet → ~7.8 packets/s → ~530 B/s. Fine for ESP-NOW.
- **Vitals packet rate:** 2 Hz.
- **Heartbeat:** 1 Hz while idle/connected; suppressed while streaming
  (the data packets themselves prove liveness).

---

## 5. Sender state machine

```
            +--------+
            | BOOTING| (init i2c, sensors, gpio)
            +---+----+
                |
                v
            +--------+   HELLO from master MAC      +-----------+
            |  IDLE  |---------------------------->| CONNECTED |
            +--------+   (LED on, beep once)        +-----------+
                ^                                    |        |
                |                          CMD_START |        |
                |  heartbeat lost (3s)               v        | CMD_STOP
                |                                +-----------+|
                +--------------------------------| STREAMING |<+
                  (LED off)                      +-----------+
```

### Tasks (FreeRTOS) on the sender
- `state_task` — owns the FSM, processes inbound ESP-NOW control packets
  via a queue, drives LED + buzzer.
- `i2c_sample_task` — every 500 ms reads MAX30102 + TMP117, updates a
  shared `latest_vitals_t` (mutex).
- `ecg_sample_task` — every 4 ms reads AD8232, pushes into a ring buffer of
  32 samples; when full, posts to ECG send queue.
- `tx_task` — pulls from vitals timer + ecg queue and calls
  `espnow_comm_send(MASTER_MAC, ...)`. Only transmits while in STREAMING.
  Sends heartbeat while in CONNECTED.

### Sender NODE_ID
- Defined at compile time:
  `#define NODE_ID NODE_ID_SENDER_1` in `sender_main.c`.
- For sender #2 in the future: change this one line and re-flash. No other
  code change.

---

## 6. Master state machine

```
            +--------+
            | BOOTING|
            +---+----+
                | (peripherals + LED on + emit master_ready)
                v
            +--------------+  cmd:connect    +-------------+
            | IDLE_NO_PEER |---------------> | CONNECTING  |
            +--------------+                 +------+------+
                                                    | READY recv
                                                    v
            +--------+  hb timeout       +------------+
            |        |<----------------- | CONNECTED  |
            |IDLE_NO |                   +-----+------+
            |  PEER  |                         | cmd:start
            +--------+                         v
                ^                        +------------+
                |                        | STREAMING  |
                | cmd:stop / hb timeout  +------------+
                +-----------------------------+
```

### Tasks on the master
- `uart_rx_task` — line-buffered reader of `{"cmd":...}` JSON, posts to
  `cmd_queue`.
- `state_task` — owns FSM. Reads `cmd_queue` and an inbound ESP-NOW queue.
  Sends `HELLO` / `CMD_START` / `CMD_STOP` to sender. Tracks
  `last_seen_ms` and detects heartbeat timeout.
- `serial_out_task` — drains a `tx_queue` of strings and `printf`s them.
  Both the FSM and the ESP-NOW receive callback push JSON strings here so
  serial output is single-writer (no interleaving).

### Peer table (future-proofing for 2 senders)
```c
typedef enum { SLOT_FREE, SLOT_PAIRED, SLOT_CONNECTED, SLOT_STREAMING } slot_state_t;

typedef struct {
    uint8_t       mac[6];
    uint8_t       node_id;
    slot_state_t  state;
    uint32_t      last_seen_ms;
} peer_slot_t;

#define MAX_PEERS 2
static peer_slot_t peers[MAX_PEERS];
```
Initialised at boot from a hardcoded `SENDER1_MAC`. Sender #2's MAC will
be added here in the future — no other master code change required.

---

## 7. Dashboard

### Module split (existing)
- `main.py` — entrypoint, port detection, queue plumbing.
- `serial_reader.py` — pyserial reader thread; pushes parsed JSON dicts into
  a queue. **Update:** also expose a `send_command(dict)` method that
  writes JSON+`\n` to the same serial port.
- `gui.py` — tkinter UI.

### UI changes
- Status bar: **Master Ready / Connected / Streaming / Disconnected**.
- Buttons: **Connect**, **Start**, **Stop**. Enabled/disabled by state.
- Panels: **rendered dynamically** keyed on `node_id` from incoming events
  (drop the hardcoded `[1, 2]`). Today only node 1 will appear; when
  sender #2 is added, a second panel appears automatically.
- Per-panel display: HR (computed from IR/RED in Python), SpO₂, body temp,
  optional ECG live plot (matplotlib in a Tk canvas — Stage 4).

### Hardcoded MAC addresses
- Sender MAC: `C0:CD:D6:CE:27:58` — hardcoded in `master_main.c` as `SENDER1_MAC`.
- Master MAC: `9C:13:9E:90:C6:E0` — hardcoded in `sender_main.c` as `MASTER_MAC`.
- Verify the actual MACs of the two DevKit V1 boards we'll use and update.

---

## 8. Stage-by-stage build plan

### Stage 0 — Cleanup
- Fix `xTaskcreate()` typo at `sender_main.c:58`.
- Remove `PEER_SENDER_MAC` references and `ping_task` from sender (not
  needed in 1-sender prototype; can re-add in Stage 6).
- Remove `gas_ppm`, `env_temp`, peer-RSSI fields from `sensor_packet_t`
  (not used; replace with `vitals_packet_t` in Stage 1).
- Confirm clean build on both nodes with current dummy data path.

### Stage 1 — Protocol + UART command channel
- Add new packet types and structs to `protocol.h`.
- Add `vitals_packet_t`, `ecg_packet_t`, `ctrl_packet_t`.
- Master: add `uart_rx_task` reading line JSON; add command queue; emit
  `{"type":"status","state":"master_ready"}` on boot.
- Master: tag all outgoing JSON lines with `"type"`.
- Dashboard: add `send_command()` method on `SerialReader`.
- Dashboard: dispatch incoming JSON by `"type"`.
- Dashboard: stub Connect / Start / Stop buttons that send commands.
- **Test:** clicking buttons causes master to print the command back.

### Stage 2 — Handshake + state machines + LED/buzzer
- Implement sender FSM with HELLO/READY, LED, buzzer beep on connect.
- Implement master FSM with hardcoded peer slot.
- Heartbeat in CONNECTED state, 3-second timeout for disconnect.
- Master emits `status` events on every transition.
- Still using dummy sensor data here.
- **Test:** Connect button → LED on + beep on sender + status update on
  dashboard. Disconnect (power-cycle sender) → status reverts within ~3s.

### Stage 3 — Real sensor data
- Wire MAX30102 + TMP117 reads into `latest_vitals_t` (mutex-guarded).
- Wire AD8232 reads into a 32-sample ring buffer → `ecg_packet_t`.
- Streaming path: vitals at 2 Hz, ECG at 250 Hz packed into ~8 packets/s.
- Master forwards both packet types as separate JSON line types.
- Dashboard parses both.
- **Test:** real values shown; finger-on-MAX30102 changes IR/RED;
  electrodes-off flag visible; no flooding.

### Stage 4 — Dashboard polish
- Dynamic per-node panels.
- State-aware button enable/disable.
- ECG live plot (matplotlib in Tk).
- Optional CSV logging per session.

### Stage 5 — Two-sender enablement (when 2nd sender ESP arrives)
- Add `SENDER2_MAC` constant in `master_main.c`, register second peer slot.
- Flash second DevKit with `NODE_ID = NODE_ID_SENDER_2`. **No other
  code change.**
- Dashboard auto-shows a second panel from the dynamic rendering done in
  Stage 4.

### Stage 6 — Optional polish
- Re-introduce inter-sender RSSI proximity buzzer (only useful with 2
  senders).
- Persist last paired sender MAC in NVS for auto-reconnect.
- Low-battery / disconnect alerts on dashboard.
- HR / SpO₂ algorithm tuning in Python.

---

## 9. Open items / decisions on hold

- **Logging vs JSON on UART0:** taking the "dashboard skips non-JSON"
  approach for now. Revisit if log volume corrupts data flow.
- **HR/SpO₂ computation location:** Python-side (per README). ESP only
  ships filtered raw IR/RED.
- **PCB layout:** out of scope for this plan. Tracked separately.

---

## 10. Glossary of new files / changes per stage

| Stage | Files touched |
|-------|---------------|
| 0 | `sender_node/main/sender_main.c` |
| 1 | `common/protocol/include/protocol.h`, `master_node/main/master_main.c`, `dashboard/serial_reader.py`, `dashboard/gui.py` |
| 2 | `sender_node/main/sender_main.c`, `master_node/main/master_main.c`, new `sender_node/components/led/` (or inline GPIO) |
| 3 | `sender_node/main/sender_main.c` (real sensor wiring) |
| 4 | `dashboard/gui.py` (matplotlib plot, dynamic panels) |
| 5 | `master_node/main/master_main.c` (one-line MAC add), sender re-flash with new `NODE_ID` |
