# Local Life-Alert — firmware

Two ESP32-WROOM dev boards talking over ESP-NOW. A button (TX, deep-sleeping)
triggers a latching alarm at a receiver (RX, always awake). No router, no cloud.
See `life-alert-firmware-guide.md` for the design rationale; this README covers
how to actually build, flash, and test the current code.

> Not a certified medical device. Do not make it someone's only safety net.

## Layout

```
tx/   transmitter (button)   -> src/main.cpp, src/common.h, platformio.ini
rx/   receiver  (alarm)      -> src/main.cpp, src/common.h, platformio.ini
```

`src/common.h` is the shared wire protocol and MUST be byte-identical in both.

## Toolchain

PlatformIO Core, installed in its own venv at `~/.platformio`. The `pio` binary is:

```
C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe
```

Both `platformio.ini` files pin the **pioarduino** platform release `55.03.39`
(Arduino-ESP32 **core 3.3.9** / ESP-IDF 5.5.4). This matters: the official
`espressif32` PlatformIO platform is stuck on core 2.x, whose ESP-NOW callback
signatures differ and will not compile this code. Core >= 3.3.0 is required so
that the recv (`esp_now_recv_info_t`) and send (`wifi_tx_info_t`) callback
signatures both match.

> **Run PlatformIO from PowerShell (or cmd), NOT Git Bash/MSYS.** The pioarduino
> platform's `idf_tools.py` aborts with "MSys/Mingw is not supported", which leaves
> the toolchain half-installed and the build fails to find `xtensa-esp-elf-g++`.

### Build (PowerShell)

```powershell
$PIO = "C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe"
& $PIO run -d tx
& $PIO run -d rx
```

### Flash + serial monitor (per board, PowerShell)

```powershell
& $PIO run -d rx -t upload          # upload to the board on the auto-detected port
& $PIO device monitor -b 115200     # watch serial; add --port COMx if needed
```

## First-time MAC bootstrap

ESP-NOW addresses peers by MAC, so each board needs the other's MAC hardcoded.

1. Flash either sketch to each board and open the serial monitor.
2. Each prints its own MAC on boot (`TX MAC ...` / `RX MAC ...`).
3. Put the **RX** MAC into `RX_MAC[]` in `tx/src/main.cpp`, and the **TX** MAC
   into `TX_MAC[]` in `rx/src/main.cpp`, as `{0xAA,0xBB,...}`.
4. Re-flash both. Confirm `ESPNOW_CHANNEL` is identical in both `common.h`.

## Testing without peripherals

Right now no buzzer / external LEDs are wired, so:

- **TX feedback** is the onboard LED (GPIO2): 2 quick blinks = confirmed,
  6 long blinks = FAILED. Plus a serial line (`CONFIRMED`/`FAILED`).
- **RX feedback** is the serial log (every state transition is printed) and the
  onboard LED, which mirrors the state (solid = ALARM, double-blink = OFFLINE,
  occasional blip = LOW-BATTERY, brief blink/3s = IDLE).
- The **BOOT button** is both the TX trigger and the RX alarm-clear button.

`HEARTBEAT_SECONDS` is set to **20** in both projects for fast testing
(offline fires after ~60 s). Raise both to 300 for real use.

### Exercising each requirement

| Test | How |
|---|---|
| Happy path | Press TX BOOT → RX logs `ALERT ... LATCHED`, onboard LED solid; TX blinks 2× + prints `CONFIRMED`. |
| Latching | Alarm stays after releasing the button; only RX BOOT clears it. |
| Failure feedback | Power RX off, press TX BOOT → TX does the 6-blink pattern + prints `FAILED`. |
| Reset safety | Hold TX BOOT and tap EN (reset) → must NOT alarm (boots to heartbeat, or download mode — see caveat). |
| Offline | Power TX off → within ~60 s RX logs `state -> OFFLINE`. |
| Low battery | Build TX with `FAKE_BATTERY_MV 3200` → RX logs `state -> LOW-BATTERY`. |

### Strapping-pin caveat (BOOT button as trigger)

GPIO0 is a strapping pin. If the TX resets/browns-out *while BOOT is held*, it
may enter download mode and simply not run (it won't false-alarm, but it can
silently fail to boot). Fine for the bench; for the permanent worn unit, move
the trigger to a plain RTC GPIO (e.g. GPIO33) and update `BUTTON_GPIO`.

## Enabling battery sense later

In `tx/src/main.cpp`: wire a divider to an ADC1 pin (GPIO32–39, default 34),
set `BATTERY_SENSE_ENABLED 1`, set `BATTERY_DIVIDER = (R1+R2)/R2`, calibrate
against a multimeter, then set `LOW_BATT_MV` (in `rx/`) with margin above the
battery's cutoff.
