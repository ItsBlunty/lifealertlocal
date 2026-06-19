# CLAUDE.md — Local Life-Alert firmware

**Before doing anything in this project, read [`HANDOFF.md`](./HANDOFF.md)** — it is
the authoritative status/context document (board MACs & COM ports, toolchain
gotchas, test results, what's done vs pending). Then [`README.md`](./README.md) for
build/flash/test mechanics and `life-alert-firmware-guide.md` for the original design.

This is a two-ESP32 ESP-NOW life-alert: a deep-sleeping button (**`tx/`**) triggers a
latching alarm on an always-awake receiver (**`rx/`**). Keep `src/common.h` identical
in both projects.

## Operational rules that cause failures if ignored

- **Run PlatformIO from PowerShell, NEVER Git Bash/MSYS.** pioarduino's
  `idf_tools.py` aborts under MSYS and the toolchain fails. Binary:
  `C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe` (call it `$PIO`).
- **Set `$env:PYTHONIOENCODING="utf-8"` before any command that redirects output to
  a file**, or esptool's output thread crashes with `UnicodeEncodeError`.
- **Platform is pinned** to pioarduino `55.03.39` (Arduino core 3.3.9). Do not
  switch to the official `espressif32` platform — it's core 2.x and won't compile
  the ESP-NOW callbacks. Both callback signatures need core ≥ 3.3.0.
- **The RX serial is the source of truth; the TX serial is garbage while it deep-
  sleeps.** Diagnose by watching the RX (or the TX's blue LED on GPIO2), not TX serial.
- COM ports change on replug — re-check with `& $PIO device list`.
- A `tool-esptoolpy.broken` dir in `~/.platformio/packages` is ACL-locked and
  prints harmless "access denied" warnings; ignore it.

## Build / flash (PowerShell)

```powershell
$PIO = "C:\Users\vhdbl\.platformio\penv\Scripts\platformio.exe"
$env:PYTHONIOENCODING = "utf-8"
& $PIO run -d tx                                # build
& $PIO run -d rx -t upload --upload-port COM3   # flash (RX on COM3, TX on COM5 — verify ports)
```

## Current state (see HANDOFF.md §6 for details)

Firmware written, paired (MACs hardcoded), and **verified on hardware**: E2E
alert+ack, latching, clear button, failure feedback (6 blinks), offline detection,
reset-safety, 20s heartbeats. **Pending:** low-battery test, buzzer/peripheral
wiring, battery divider, raising `HEARTBEAT_SECONDS` to 300, moving the trigger off
the GPIO0 strapping pin for the permanent build.

When status changes, update `HANDOFF.md` (keep it the single source of truth).
