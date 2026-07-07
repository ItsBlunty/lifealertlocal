# Local Life-Alert — Project Handoff / Status

Context-reset summary as of **2026-07-07**. Read this first, then `README.md`
(build/flash/test mechanics) and `life-alert-firmware-guide.md` (original design).
This file captures everything not obvious from the code.

---

## 1. What this is

ESP-NOW link between a button (**TX**, deep-sleeping) and a **latching alarm** at a
**RX** (always awake). No router/cloud.

- **RX** is an ESP32-WROOM dev board (reuses its BOOT button / GPIO0 as the alarm-clear button).
- **TX** was migrated from an ESP32-WROOM to a **Seeed XIAO ESP32-S3** (2026-07-06).
  The button is now an **external switch on pad D0 / GPIO1 to GND** (no longer the
  BOOT strapping pin — see §7). Verified working on WROOM; **XIAO-S3 TX bring-up now
  done (2026-07-07): flashed, MAC paired into `rx/`, heartbeat link confirmed live.**

Status: **firmware written, both images build, RX + XIAO-S3-TX pairing verified on
hardware** (heartbeats received, `seq` increments across deep-sleep cycles). No
peripherals (buzzer/external LEDs/battery divider) wired yet — verification is via
serial + the onboard LED. **Not yet re-run on the XIAO: physical button ALERT +
2-blink/6-blink feedback, and offline-recovery (see §6/§8).**

---

## 2. The two boards

| Role | Board | STA/base MAC | Current COM port | Notes |
|------|-------|--------------|------------------|-------|
| **RX** (receiver/alarm) | ESP32-WROOM #1 | `44:1D:64:F5:87:F8` | COM3 | flashed `rx/` |
| **TX** (button/transmitter) | XIAO ESP32-S3 | `E0:72:A1:F9:54:1C` | (vanishes on sleep; COM12 when awake) | flashed `tx/`; paired into `rx/`'s `TX_MAC[]` 2026-07-07. `EC:E3:34:1A:64:FC` was the OLD WROOM TX |

- **RX** is ESP32-WROOM with **Silicon Labs CP210x (CP2102)** USB-UART (VID:PID
  10C4:EA60); blue LED on GPIO2 (solid when latched, periodic blink when idle).
- **TX (XIAO ESP32-S3)** uses **native USB-CDC** (no CP2102) — different USB
  VID:PID, and **the COM port vanishes while it deep-sleeps.** Onboard user LED is
  **GPIO21, active-LOW** (code handles the inversion). First flash may need manual
  bootloader entry (hold BOOT, tap RESET, release BOOT).
- **The XIAO's MAC differs from the old WROOM TX.** On first flash, read the new TX
  MAC from the `TX MAC:` serial line (or esptool's `MAC:`) and paste it into
  `TX_MAC[]` in `rx/src/main.cpp`, then reflash RX. `RX_MAC[]` in `tx/` is unchanged.
- **COM ports can change** on replug — always re-check with `& $PIO device list`.

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
- **TX serial is unreliable while it deep-sleeps** — the port vanishes between wakes.
  Do NOT diagnose the TX from its own serial. Watch the RX instead, or watch the TX's LED.
- **Opening the XIAO's native-USB port RESETS the chip** (`rst:0x15
  USB_UART_CHIP_RESET`). So you cannot passively monitor the TX — the act of opening
  COM12 reboots it and destroys the state you were trying to observe. Diagnose from RX.
- **Two WROOM boards are indistinguishable** (both now run `rx/`, same CP210x
  `SER=0001`, both show as COM3). This mix-up has bitten twice — a "link down" that
  survived power-cycles turned out to be the wrong WROOM (`EC:E3…` spare) plugged in
  instead of the real RX (`44:1D…`). **When a hardcoded-MAC link "can't see each other,"
  READ THE ACTUAL MAC of the connected board FIRST** (flash it, or reset it and read the
  `RX MAC:` boot line) before suspecting code/RF. **LABEL the boards.**
- A quick, non-destructive way to force an RX boot banner (prints its MAC + channel):
  open COM3, pulse `DtrEnable=$false; RtsEnable=$true; sleep 150ms; RtsEnable=$false`.
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
- **TX button is debounced (both edges).** An EXT1 wake becomes an ALERT only after a
  stable-LOW confirm (`DEBOUNCE_MS`/`PRESS_CONFIRM_MS`); we then wait for a debounced
  RELEASE (`RELEASE_WAIT_MS`) before sleeping. Rejects contact-bounce glitches and was
  the fix for the stuck-awake wedge (§6).
- **TX only arms the EXT1 wake when the pad is released (HIGH).** Arming wake-on-LOW
  while the pad is still LOW makes deep sleep wake instantly → the flood. If held, it
  sleeps timer-only and re-arms on a later released wake.
- **ALERT never gives up + is flash-persisted.** Retries forever (awake) until app-ACK;
  the pending flag/seq live in NVS (`Preferences` "lifealert") so any reset — incl. full
  power loss — resumes it. Clears only on delivery. See §6 for the full behaviour + the
  "can't clear by power-cycle" consequence.
- **TX wakeup is EXT1 (ANY_LOW), not EXT0.** The ESP32-S3 has no EXT0 peripheral, so
  the XIAO port uses `esp_sleep_enable_ext1_wakeup()` with an RTC pull-up held through
  sleep; the wake-cause check is `ESP_SLEEP_WAKEUP_EXT1`. Button is D0/GPIO1 to GND.
  (Battery ADC note: ADC1 on the S3 is GPIO1-10, not the WROOM's GPIO32-39.)
- **Both boards print their WiFi channel at boot** (`[tx]/[rx] wifi channel = 1`) — a
  leftover diagnostic from the board-swap hunt; handy to confirm both are on ch 1.
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

### XIAO-S3 re-verification (2026-07-07)
- **#1/#3 ALERT + latch — ✅ on XIAO.** Momentary short of D0/GPIO1→GND → XIAO EXT1
  wake → RX `alarm LATCHED` + ACK → **XIAO 2-blink confirm** (user saw two flashes) →
  RX status LED **solid** (ST_ALARM), persists. Heartbeat cadence ✅ clean single
  frames every ~20s, `seq` increments across deep-sleep.
- **#6 reset-safety re-confirmed:** power-cycling the XIAO cold-boots to `heartbeat
  seq=1` (RTC cleared), never an ALERT.

### ✅ RESOLVED — XIAO "heartbeat flood / stuck-awake" wedge
- Symptom (was): after a bouncy manual short, the XIAO stopped deep-sleeping and
  **continuously re-transmitted a SINGLE heartbeat** — RX logged the same `seq` 100k+
  times (`seq=45 (x117134)`). Root cause: a still-LOW / bouncing button pad at sleep
  time, so `esp_sleep_enable_ext1_wakeup(ANY_LOW)` woke the chip immediately, over and over.
- **Fix (in `tx/`, 2026-07-07):** (a) `goToSleep()` now only arms the EXT1 button wake
  when the pad reads HIGH (released); if still LOW it sleeps on the **heartbeat timer
  only** and re-arms once released. (b) A debounce layer: an EXT1 wake becomes an ALERT
  only if the pad is stably LOW for `DEBOUNCE_MS` within `PRESS_CONFIRM_MS` (rejects
  bounce glitches → heartbeat), and we wait for a debounced RELEASE before sleeping.
  Verified: clean single heartbeats at 20 s, no flood, across many cycles.
- Also fixed on **`rx/`**: heartbeat logging now **dedups** (repeated same-`seq` frames
  collapse to one line + `(suppressed N duplicate ...)`), so any future flood can't drown
  the serial log. Liveness + ACK still act on every frame.

### ✅ NEW — alert is now "never give up" + survives any reset (2026-07-07)
- **Continuous retry:** a confirmed press retries the ALERT *forever* (stays awake, no
  sleep) until the RX app-ACKs, giving the urgent 6-blink each undelivered round. Battery
  trade-off accepted by design (USB-powered today; see roadmap for battery).
- **Reset/brownout/power-loss persistence:** the undelivered-alert flag + its seq are
  stored in **FLASH (NVS)** (`Preferences` "lifealert"), written on press (before any
  send) and cleared on delivery. So a brownout, watchdog, EN/RESET button, or **full
  power-off mid-alert** all resume the alert on reboot — it is never lost. (Earlier
  attempt used `RTC_DATA_ATTR`, which the startup code re-inits on non-deep-sleep resets,
  and RTC is wiped by the EN button anyway; flash fixed both.) `seqCounter` (heartbeats
  only) stays in `RTC_NOINIT_ATTR` + magic guard.
- **Verified on hardware:** RX off → press → 6-blink retry → tap XIAO RESET → **resumed**
  6-blinking (flash survived) → plug RX in → delivered `ALERT seq=2 → LATCHED` + 2-blink,
  flag cleared, back to heartbeats. (Tell-tale: delivered alert `seq=2` from flash, next
  heartbeat `seq=1` from wiped RTC.)
- **Behavioural consequence (intended):** a genuinely-triggered-but-undelivered alert
  **cannot be cleared by power-cycling the TX** — only by delivery to the RX (then a human
  clears at the RX) or a reflash.

### Still pending
- **#5 Low-battery warning** — NOT tested on hardware. Set `FAKE_BATTERY_MV 3200`
  in `tx/`, reflash, expect RX `state -> LOW-BATTERY`.
- **Audible distinctness** of the 3 warning states — no buzzer wired; only the
  millis-based patterns are coded (GPIO25 active buzzer).
- **Range test** (guide §10.9) — not done.

---

## 7. KEY real-world lesson: BOOT-as-trigger / strapping pin  (RESOLVED for TX)

Historical (WROOM TX prototype):
- GPIO0 is a strapping pin. **Holding BOOT while resetting the TX put it into
  download mode — it stopped running entirely** (no LED, no radio) until a clean
  reset / power cycle. This caused a false "failure feedback doesn't work" scare;
  after a normal reset it worked (6 blinks).
- Because BOOT *was* the trigger, "hold BOOT during reset" was indistinguishable from
  a real press once asleep. You could not cleanly test "reset with trigger held."

**Resolved on the XIAO S3 (2026-07-06):** the trigger is now an external button on
the dedicated **RTC pad D0/GPIO1** (EXT1 wake), off any strapping pin — so the
prototype hazard above no longer applies. The XIAO's own BOOT button is only used
for entering the flash bootloader. (RX clear button can still be any GPIO.)

---

## 8. Next steps / roadmap

1. Test low-battery (#5) via `FAKE_BATTERY_MV 3200`.
2. Wire peripherals: active buzzer (GPIO25), alarm LED (GPIO26), optional TX
   vibration motor (via transistor); verify the 3 warning states are audibly distinct.
3. Battery divider on ADC1 (GPIO34): set `BATTERY_SENSE_ENABLED 1`,
   `BATTERY_DIVIDER = (R1+R2)/R2`, calibrate vs multimeter, set `LOW_BATT_MV`.
4. Raise `HEARTBEAT_SECONDS` to 300 in **both** projects.
5. Permanent build: ~~dedicated trigger GPIO (off BOOT)~~ **done on XIAO (D0/GPIO1)**;
   enclosure, RX power backup (mains-only today = alarm dead on power loss), optional
   ESP-NOW encryption.
6. **XIAO S3 bring-up:** ✅ **DONE 2026-07-07** — TX MAC `E0:72:A1:F9:54:1C` paired into
   `rx/`, heartbeat link verified, physical-button ALERT + 2-blink/6-blink feedback +
   flash-persisted never-give-up retry all verified on hardware (§6).
7. **LABEL the two WROOM boards** — `RX 44:1D` (the paired receiver, COM3) vs
   `SPARE EC:E3` (old TX, now also runs `rx/`). They're identical and the mix-up cost
   real debugging time twice (§4). Optionally flash the spare with something obviously
   different so it can't masquerade as the RX.
8. **Retire the boot-time `wifi channel` diagnostic prints** in both `main.cpp` once
   you're confident — they were added to hunt the (mis-diagnosed) link-down.

---

## 9. Git

Repo initialized; firmware committed and verified. Monitor/upload `*.log` files and
`get-platformio.py` are gitignored. Latest meaningful commit (2026-07-07): XIAO-S3 TX
bring-up + button debounce/anti-wedge + never-give-up flash-persisted ALERT + RX
heartbeat-log dedup, all verified on hardware.
