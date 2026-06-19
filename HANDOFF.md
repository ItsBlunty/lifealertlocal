# Local Life-Alert — Project Handoff / Status

Context-reset summary as of **2026-06-19**. Read this first, then `README.md`
(build/flash/test mechanics) and `life-alert-firmware-guide.md` (original design).
This file captures everything not obvious from the code.

---

## 1. What this is

Two ESP32-WROOM dev boards over ESP-NOW. A button (**TX**, deep-sleeping) triggers
a **latching alarm** at a **RX** (always awake). No router/cloud. Prototype reuses
the **BOOT button (GPIO0)** as the TX trigger and the RX alarm-clear button.

Status: **firmware written, both images build, paired, and verified working on
hardware.** No peripherals (buzzer/external LEDs/battery divider) wired yet —
verification is via serial + the onboard blue LED (GPIO2).

---

## 2. The two boards (identical modules)

| Role | Board | STA/base MAC | Current COM port | Notes |
|------|-------|--------------|------------------|-------|
| **RX** (receiver/alarm) | #1 | `44:1D:64:F5:87:F8` | COM3 | flashed `rx/` |
| **TX** (button/transmitter) | #2 | `EC:E3:34:1A:64:FC` | COM5 | flashed `tx/` |

- Both are ESP32-WROOM with **Silicon Labs CP210x (CP2102)** USB-UART (VID:PID 10C4:EA60).
- Identical modules; **blue LED on GPIO2 works** (confirmed on RX: solid when
  latched, periodic blink when idle).
- **COM ports can change** on replug — always re-check with
  `& $PIO device list`.
- MACs are already hardcoded: `tx/src/main.cpp` `RX_MAC[]` = RX's MAC;
  `rx/src/main.cpp` `TX_MAC[]` = TX's MAC.

---

## 3. Toolchain — non-obvious requirements

- **PlatformIO Core 6.1.19**, isolated venv. Binary:
  `C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe` (alias it `$PIO`).
- **RUN FROM POWERSHELL, NOT Git Bash/MSYS.** pioarduino's `idf_tools.py` aborts
  with "MSys/Mingw is not supported", leaving the toolchain half-installed and the
  build failing to find `xtensa-esp-elf-g++`.
- **When redirecting output to a file, set `$env:PYTHONIOENCODING="utf-8"` first.**
  Otherwise esptool's progress chars crash the output thread with
  `UnicodeEncodeError` (cp1252) — this aborted a flash mid-write once.
- Platform is **pinned**: pioarduino `55.03.39` → **Arduino core 3.3.9 / ESP-IDF
  5.5.4**. Required: the ESP-NOW recv (`esp_now_recv_info_t`) and send
  (`wifi_tx_info_t`) callback signatures only align at **core ≥ 3.3.0**. The
  official `espressif32` platform is stuck on core 2.x and will NOT compile this.
- **Harmless leftover:** `C:\Users\vhdbl\.platformio\packages\tool-esptoolpy.broken`
  is ACL-locked (can't be deleted, even with takeown). It prints "Access is denied"
  warnings on every run. Ignore it — builds/uploads succeed regardless.

### Commands (PowerShell)
```powershell
$PIO = "C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe"
$env:PYTHONIOENCODING = "utf-8"
& $PIO run -d "C:\Users\vhdbl\Downloads\LifeAlertLocal\tx"                              # build TX
& $PIO run -d "C:\Users\vhdbl\Downloads\LifeAlertLocal\rx" -t upload --upload-port COM3 # flash RX
```

---

## 4. Reading serial — gotchas

- **RX serial is clean and is the source of truth** (mains/always awake). It logs
  every state transition (`state -> ALARM/OFFLINE/LOW-BATTERY/IDLE`) and every
  received message (`heartbeat`/`ALERT` with `seq` and `batt`).
- **TX serial is unreliable while it deep-sleeps** — the floating TX pin produces
  either a flood of garbage or near-silence. Do NOT diagnose the TX from its own
  serial. Watch the RX instead, or watch the TX's LED.
- ESP32 **ROM boot messages are at 74880 baud**; the app is 115200. Garbage when
  reading at 115200 usually means the board is resetting/looping (ROM noise).
- To read a board's MAC reliably: esptool prints `MAC: xx:xx:...` during every
  flash — for ESP32 that equals the WiFi STA MAC.
- Monitoring pattern used (PowerShell .NET SerialPort): open `COMx,115200`, loop
  `ReadLine()` appending to a log file for N seconds. Opening COM3 does **not**
  reset the RX (verified). Monitor logs (`*_monitor.log`, `upload.log`) are
  gitignored.

---

## 5. Firmware design & decisions (deviations from the guide are flagged in code)

- Two projects `tx/` + `rx/`, identical `src/common.h` (protocol `Message` struct,
  `PROTO_VERSION 1`, `ESPNOW_CHANNEL 1`).
- **`HEARTBEAT_SECONDS = 20` in BOTH** — a TESTING value. **TODO: set to 300 for
  deployment** (must match in both projects).
- **Battery sense is OFF**: `BATTERY_SENSE_ENABLED 0`, `FAKE_BATTERY_MV 0` →
  `battery_mv = 0` = "N/A", so RX never false-warns. To test low-battery, set
  `FAKE_BATTERY_MV 3200` in `tx/src/main.cpp` and reflash.
- **RX** logs all transitions/messages and **mirrors state on the onboard LED**
  (solid=ALARM, double-blink=OFFLINE, blip=LOW-BATT, brief/3s=IDLE) since no buzzer
  is wired. Buzzer (GPIO25) + alarm LED (GPIO26) code is present for later.
- **TX reads its MAC AFTER `esp_now_init()`** (fixed a bug where reading right after
  `WiFi.mode()` returned `00:00:00:00:00:00`).
- **TX waits for button release before sleeping** to avoid alert spam from a held
  button.
- Offline threshold = `HEARTBEAT_SECONDS * OFFLINE_MULT(3)` = ~60s with current settings.

---

## 6. Acceptance tests — results on hardware

| # | Requirement | Result |
|---|-------------|--------|
| 1 | End-to-end confirmation (ACK before feedback) | ✅ ALERT acked by RX (round trip); RX latches + acks |
| 2 | Distinct failure feedback | ✅ RX off → press → **6 slow blinks** on TX LED |
| 3 | Latching alarm (momentary press → persistent) | ✅ Latched, persisted until cleared |
| — | Clear button (RX BOOT) | ✅ `alarm CLEARED by button` → IDLE |
| 4 | Liveness watchdog (offline) | ✅ `state -> OFFLINE` ~60s after heartbeats stop; recovers to IDLE when TX returns |
| 6 | Reset must never fire alarm | ✅ Power-on/replug → heartbeat (`seq=1`), not ALERT |
| — | Heartbeat cadence | ✅ every 20s, incrementing `seq`, received + acked |

### Still pending
- **#5 Low-battery warning** — NOT tested on hardware. Set `FAKE_BATTERY_MV 3200`
  in `tx/`, reflash, expect RX `state -> LOW-BATTERY`.
- **Audible distinctness** of the 3 warning states — no buzzer wired; only the
  millis-based patterns are coded (GPIO25 active buzzer).
- **TX 2-blink confirm** — proven logically (RX latched + acked) but not visually
  confirmed by the user.
- **Range test** (guide §10.9) — not done.

---

## 7. KEY real-world lesson: BOOT-as-trigger / strapping pin

- GPIO0 is a strapping pin. **Holding BOOT while resetting the TX put it into
  download mode — it stopped running entirely** (no LED, no radio) until a clean
  reset / power cycle. This caused a false "failure feedback doesn't work" scare;
  after a normal reset it worked (6 blinks).
- Because BOOT *is* the trigger, "hold BOOT during reset" is indistinguishable from
  a real press once asleep — so it will (correctly) alert via EXT0. You cannot
  cleanly test "reset with trigger held" on this prototype.
- **Permanent build:** move the trigger to a dedicated **plain RTC GPIO (e.g.
  GPIO33)** with an external button, update `BUTTON_GPIO`. (RX clear button can be
  any GPIO.)

---

## 8. Next steps / roadmap

1. Test low-battery (#5) via `FAKE_BATTERY_MV 3200`.
2. Wire peripherals: active buzzer (GPIO25), alarm LED (GPIO26), optional TX
   vibration motor (via transistor); verify the 3 warning states are audibly distinct.
3. Battery divider on ADC1 (GPIO34): set `BATTERY_SENSE_ENABLED 1`,
   `BATTERY_DIVIDER = (R1+R2)/R2`, calibrate vs multimeter, set `LOW_BATT_MV`.
4. Raise `HEARTBEAT_SECONDS` to 300 in **both** projects.
5. Permanent build: dedicated trigger GPIO (off BOOT), enclosure, RX power backup
   (mains-only today = alarm dead on power loss), optional ESP-NOW encryption.

---

## 9. Git

Repo initialized; firmware committed and verified. Monitor/upload `*.log` files and
`get-platformio.py` are gitignored. Latest meaningful commit wires the paired MACs
and the TX MAC-read fix.
