# Local Life-Alert — Project Handoff / Status

Context-reset summary as of **2026-07-22**. Read this first, then `README.md`
(build/flash/test mechanics) and `life-alert-firmware-guide.md` (original design).
This file captures everything not obvious from the code.

---

## 1. What this is

ESP-NOW link between a button (**TX**, deep-sleeping) and a **latching alarm** at a
**RX** (always awake). No router/cloud.

- **RX** is an ESP32-WROOM dev board (reuses its BOOT button / GPIO0 as the alarm-clear button).
- **TX** was migrated from an ESP32-WROOM to a **Seeed XIAO ESP32-S3** (2026-07-06).
  The button is now an **external switch on pad D9 / GPIO8 to GND** (moved from D0/GPIO1
  on 2026-07-07 for easier wiring; still RTC-capable so EXT1 wake is unchanged — no longer
  the BOOT strapping pin, see §7). Verified working on WROOM; **XIAO-S3 TX bring-up now
  done (2026-07-07): flashed, MAC paired into `rx/`, heartbeat link confirmed live.**

Status: **firmware written, both images build, RX + XIAO-S3-TX pairing verified on
hardware** (heartbeats received, `seq` increments across deep-sleep cycles). Battery divider
wired + calibrated on the TX; buzzer still not wired. Both boards now drive an addressable
WS2812 for visible state: the **TX** pixel on D1/GPIO2 (green/red feedback + blue low-batt +
boot voltage readout), and a **new RX** pixel on GPIO4 (rainbow strobe on alarm, R-R/G-G/B-B
on offline). **Latest verified build 2026-07-22 (see the 2026-07-22 section below); TX cell ≈ 3.9 V.**

### 2026-07-22 changes (RX external rainbow/offline LED + faster offline + TX low-batt blue blip + TX boot battery readout)
Both images build clean. Board states: **RX flashed** (COM3) with all the RX items below; **TX flashed
production** (COM12 in bootloader) with both TX items below. Current TX cell ≈ **3.9 V** (read via the new
boot readout LED — above the 3.4 V low-batt threshold, so no blue blip in normal use right now).

- **RX external WS2812 NeoPixel on GPIO4 — ✅ BOTH MODES VERIFIED ON HARDWARE.** A second addressable
  pixel (same `neopixelWrite()` idiom as the TX; DIN→GPIO4, VDD→3V3, GND→GND) gives loud visible alarm
  state on the always-awake RX:
  - **ALARM latched → bright RAINBOW STROBE** (~8 Hz, hue sweep ≈1.5 s). `hsvWheel()` + a cached
    `setPixel()` (only writes on color change, so the RMT isn't bit-banged every fast loop).
  - **OFFLINE → Red-Red / Green-Green / Blue-Blue, repeating** (6 half-on/off slots @ `OFFLINE_SLOT_MS`
    220 ms) so you can tell *at the RX* the TX has gone missing. `PIXEL_LEVEL` 255 (mains-powered, no
    drain worry). Idle/low-batt → dark. Onboard STATUS LED (GPIO2) still mirrors state as a redundant cue.
- **RX offline threshold cut 900 s → 610 s.** Replaced `OFFLINE_MULT` with a direct `OFFLINE_SECONDS 610`
  = two 300 s heartbeats + 10 s margin (15 min was too long). `offline = (now-lastHb) > OFFLINE_SECONDS*1000`.
- **TX low-battery BLUE blip (LOWEST-priority LED mode) — ⚠️ built + flashed, NOT hardware-verified**
  (cell is 3.9 V > 3.4 V, so it can't trigger without a fake-low build or a real sag). When batt < `LOW_BATT_MV`
  3400: one **blue** flash on the idle/link-healthy path, then deep-sleep only `LOW_BATT_SLEEP_SEC` 10 s
  (vs 300 s) → ~one blue blip every 10 s, chip asleep in between, heartbeating each wake. It lives ONLY in
  the link-healthy+no-alert branch, so GREEN (alerting) and RED (link down/undelivered) always override it
  (spec: "anything but Off wins"). Self-healing: ≥3.4 V → no blue, back to 300 s. To test: `FAKE_BATTERY_MV
  3200` + `BATTERY_SENSE_ENABLED 0`, reflash, expect blue every ~10 s (and the RX low-batt path too).
- **TX boot-only battery-VOLTAGE readout on the pixel — ✅ VERIFIED (user read 3.9 V off the LED).** Runs
  ONCE on a true RESET/power-on (`ESP_SLEEP_WAKEUP_UNDEFINED`, not a timer/EXT1 wake) and is skipped while
  an alert is pending (alert shows immediately, never waits behind the diagnostic). Sequence: **5 PURPLE**
  ("readout coming"), then **3 rounds** of **ORANGE × volts-ones-digit** then **BLUE × tenths-digit**
  (voltage rounded to 0.1 V; e.g. 3.9 V = 3 orange + 9 blue, ×3). ~15-20 s total; delays the first
  post-reset heartbeat by that long (harmless). **Caveat: a digit of 0 (exactly x.0 V, or N/A when sense
  is off) shows as NO flashes for that digit.**
- **Uncommitted:** all four items above are in the working tree, not yet committed.

### 2026-07-17 changes (LED→two states + "clear-to-stand-down" protocol + link-down dropped-press FIX) — ✅ TX reflashed 2026-07-22; **link-down dropped-press FIX + 10 ms debounce VERIFIED ON HARDWARE**
- **TX LED reduced to TWO states + off** (was green-2-blink / red-6-blink / rainbow):
  - **GREEN repeated blink = "someone is coming"** (alert received & the RX alarm is latched/active).
  - **RED repeated blink = "something is wrong"** (RX unreachable: alert unacked OR heartbeat link-down).
  - Wearer only has to learn the color, not the pattern. Shared cadence `BLINK_ON_MS/OFF_MS=250`,
    `BLINK_BURST=6` per round. **The link-down RAINBOW is REMOVED** (rainbowSweep/hsvToRgb/
    `RAINBOW_LEVEL`/`RAINBOW_SWEEP_MS` deleted) — link-down is now just the red blink. Supersedes the
    2026-07-10 rainbow entry below.
- **NEW alert protocol — TX stays awake & GREEN until a human CLEARS at the RX, then a 50-blink escort:**
  - While an alert is active the TX **never sleeps**. It re-sends the (fixed-seq) ALERT in a loop and,
    per round: acked+active → **green** burst; unacked → **red** burst; acked+**cleared** → stop.
  - When the RX operator presses **clear**, the RX records that seq (`clearedSeq`) and thereafter **ACKs
    that seq as cleared instead of re-latching** (the still-awake TX keeps resending the same seq — a
    genuinely new press carries a new seq and latches fresh). This is how the TX learns to stand down.
  - On learning of the clear, the TX does **`GREEN_ESCORT_BLINKS=50`** green blinks (~25 s of reassurance
    for the responder's walk over), then drops to the normal 300 s deep-sleep heartbeat cycle.
  - **Why:** lets the responder stop the TX draining its battery (it was awake for the whole alert) while
    the wearer still sees green = help is still coming. Replaces the old "ack once → 2-blink → sleep".
- **Protocol/struct:** `common.h` (identical both sides) — the spare `_pad` byte is now `flags`; a
  `MSG_ACK` carries **`ACK_ALARM_ACTIVE`** so the RX reports alarm state back to the TX. `PROTO_VERSION`
  unchanged (layout size identical). RX: `sendAck()` gained a `flags` arg; new `currentAlarmSeq`/
  `clearedSeq`; ALERT log now dedups per new seq (the awake TX re-sends every round).
- **Reset-safety preserved (req #6):** the flash (NVS) alert flag now clears **only on an operator
  clear**, never on a bare reset; a reset mid-alert resumes green-blinking. Alert seqs start at 1 so
  `clearedSeq=0` at boot can't false-match.

- **Debounce tuned to bias HARD toward catching a press (life-alert must not drop one):**
  `DEBOUNCE_MS 40 → 10` (min continuous-LOW to accept a press; ~5 poll cycles at the 2 ms read
  interval), `PRESS_CONFIRM_MS 120 → 300` (the WINDOW to *find* that stable-LOW after an EXT1 wake — a
  timeout, NOT a hold time; wider = catches a bouncy/late-settling contact, early-exits on a real press
  so nearly free). Two different knobs: DEBOUNCE_MS = "how brief a LOW counts" (lower to catch quicker
  presses); PRESS_CONFIRM_MS = "how long we keep looking" (raise to catch late/bouncy ones). Noise
  rejection is now intentionally minimal — **raise DEBOUNCE_MS if real-world noise causes false alerts.**

- **🐛 FIXED IN CODE (built, NOT yet flashed/tested) — dropped press during heartbeat link-down:**
  The old `while(!ok)` heartbeat link-down loop spun awake doing `blinkRed()`/`sendMessage()` and
  **never read the button** — EXT1 is a *deep-sleep* wake source and can't fire while awake, and nothing
  polled the pad. So a real press while the TX showed link-down RED was silently discarded; when the RX
  returned, the pending heartbeat just acked and the TX slept (no alarm). **User reproduced this on
  hardware** (firm/repeated presses ignored; confirmed the button itself works fine). The seq jump
  2→9 in the RX log was the tell: the link-down loop does `++seqCounter` each round, so ~6 heartbeat
  rounds elapsed while "red" — proof those presses went out as heartbeats, not alerts.
  **Fix:** new `blinkRedWatchButton()` blinks a red round while **continuously polling** the pad and
  returns the instant it sees a debounced LOW; new `latchAlert()` helper; `setup()` refactored into ONE
  unified awake loop so a polled press in the link-down branch calls `latchAlert()` → next iteration
  flips to the ALERT path → a **real, flash-persisted alert** that delivers + latches the moment the RX
  returns. Self-healing + reset-persistence unchanged. **TX build is clean.**
  **✅ VERIFIED ON HARDWARE 2026-07-22:** TX reflashed on COM12; ran the exact broken case — unplug RX →
  tap TX RESET (immediate link-down RED) → **press the alert button during RED** → replug RX. Result:
  **RX LED went SOLID (latched) and TX flipped RED→GREEN** — the press is now caught during link-down,
  not dropped. (Serial monitor was silent through the replug — the one-shot `ALERT … LATCHED` line raced
  ahead of the monitor open + resend dedup; the LEDs are the ground truth. See the LED-testing note.)

- **✅ E2E VERIFIED on hardware 2026-07-17 (with the two-state + clear protocol; PRE the link-down fix):**
  - Link up → TX dark (no false red). Press → TX repeated **GREEN**, RX logged `ALERT seq=2 … LATCHED`
    (logged once despite the awake TX re-sending — the per-seq dedup works), `batt≈4050`.
  - RX **clear** (BOOT) → RX `alarm CLEARED … TX will stand down` → **stayed IDLE, did NOT re-latch**
    the resends; TX did its GREEN escort → dark. ✅ clear-to-stand-down works.
  - **RED during an ACTIVE alert:** with a real latched alert, unplug RX → TX **held RED** (persisted,
    retrying); replug → TX **RED→GREEN** self-healed and RX re-latched the same `seq=9`. ✅
  - **✅ NOW VERIFIED (2026-07-22):** the link-down **dropped-press FIX** and the new **10 ms debounce** —
    TX reflashed, broken case reproduced-then-passed (RX LED SOLID + TX RED→GREEN). See the ✅ line above.

- **⏭️ NEXT SESSION:**
  1. **Housekeeping — stand the TX down:** after the 2026-07-22 retest the boards were left with a real
     latched alert (RX solid / TX green, seq=1). **Press CLEAR (BOOT) at the RX** so the TX does its green
     escort and drops back to the 300 s deep-sleep heartbeat (else it stays awake draining battery).
  2. Remaining acceptance items are the non-safety ones: **#5 low-battery** (`FAKE_BATTERY_MV 3200` in
     `tx/`, or tune `LOW_BATT_MV` to the cell), **buzzer/peripheral wiring**, and the **range test**.
  3. Retire the boot-time `wifi channel` diagnostic prints once confident (roadmap §8).
  - **Testing note:** design retests so pass/fail is visible on the **LEDs** (RX solid / TX green|red),
    not just serial — serial is timing-fragile (dedup + replug race). Monitor script this session:
    `scratchpad\rx_monitor.ps1 -Seconds N` (passive; dies on RX unplug, restart after replug).

- **Note — regression by design:** link-down RED and undelivered-alert RED are now identical (user's
  intent: "red = something wrong"). So a marginal press that falls through to a heartbeat is no longer
  visually distinct from a link blip. User is **handling the button/contact in hardware**; firmware side
  is the 10 ms debounce + the link-down press-poll fix above.

### 2026-07-12 changes (RX board swap + battery divider + physical button) — ✅ all verified on hardware
- **RX board swapped:** the `44:1D:64:F5:87:F8` WROOM is retired as RX; the former spare/old-TX
  WROOM **`EC:E3:34:1A:64:FC`** is now the RX (flashed `rx/`, COM3, MAC confirmed via esptool at
  flash time). `tx/`'s `RX_MAC[]` was retargeted `44:1D`→`EC:E3`. `rx/`'s `TX_MAC[]` (the XIAO)
  is unchanged, so `rx/` source needed no edit. **Old labels now inverted — re-label the boards:**
  `EC:E3` = RX, `44:1D` = spare.
- **Battery divider ENABLED + CALIBRATED:** `BATTERY_SENSE_ENABLED 1` in `tx/`. Reads a divider on
  **D2/GPIO3** (`BATTERY_ADC_PIN 3`). `BATTERY_DIVIDER` calibrated to **2.0306** (from 2.0) via a
  single-point trim: meter 3.85 V vs reported 3792 mV → `2.0 × 3850/3792`. Recommend 100k/100k legs
  + 0.1µF ceramic on the ADC node. TX reflashed on COM12 (MAC `e0:72:a1:f9:54:1c` confirmed).
- **✅ E2E link + battery verified 2026-07-12:** both boards flashed; XIAO RESET → RX logged
  `heartbeat seq=1 batt=…`, and the TX slept (got the ACK) rather than rainbowing — so the new
  `EC:E3` pairing works both directions. Post-calibration reading **`batt=3836` vs 3.85 V meter =
  0.36 % error** — dialed in. Divider is wired, reading, and above the 3300 threshold (no false low-batt).
- **Note — RX heartbeat-log dedup can hide repeated reads:** every XIAO RESET wipes RTC → each
  cold-boot heartbeat is `seq=1`, and the RX (§6) suppresses repeated same-`seq` frames, so only the
  FIRST `seq=1` after an RX boot prints. To re-observe a fresh `batt=`, reset the RX first (pulse
  EN via RTS, HANDOFF §4) to clear its dedup state, then tap the TX. Liveness/ACK still act on every frame.
- **Remaining battery item:** `LOW_BATT_MV` (rx/) is still the default **3300** — tune to the cell's
  cutoff if desired (low-battery path #5 still not exercised on hardware; set `FAKE_BATTERY_MV 3200`
  in tx/ to test, or just let a real cell sag).
- **✅ Physical button (D9/GPIO8 → GND) wired + working.** A real momentary button now drives the
  ALERT (previous verification used a jumper short). **Root cause of a long "button doesn't trigger"
  hunt: the D9 SIGNAL wire was not solidly connected** (loose/intermittent) — the ground leg was fine.
  Diagnostic tell that misled us: an intermittent D9 contact would still trip the EXT1 **wake**
  (a momentary dip is enough) but couldn't hold a stable LOW, so the debounce (`waitButtonStable`,
  40 ms stable-LOW within `PRESS_CONFIRM_MS`) rejected it as a glitch → the press came through as a
  **HEARTBEAT, not an ALERT** (no LED, no RX alarm). **Debug lesson for next time: symptom "press
  wakes the TX (seq increments) but never latches an ALERT" = a marginal button/contact on the SIGNAL
  side, not the code.** The definitive test is metering **D9→GND while held** (must read ~0 V, not a
  mid-rail divider voltage); a `seq` bump on the RX alone is ambiguous (EXT1-reject vs a power blip).

### 2026-07-07 changes (flashed + hardware-verified 2026-07-10)
- **`HEARTBEAT_SECONDS` = 300** (was 20) in BOTH `tx/` and `rx/` — deployment cadence.
  Consequence: offline detection is now `300*OFFLINE_MULT(3)` = **900 s / 15 min**, not 60 s.
- **TX button moved D0/GPIO1 → D9/GPIO8** (`BUTTON_GPIO GPIO_NUM_8`), RTC-capable, EXT1 unchanged.
- **TX feedback LED is now an external addressable WS2812/NeoPixel on D1/GPIO2**, replacing
  the onboard GPIO21 LED. Driven by the core's built-in `neopixelWrite()` (no lib_deps).
  **Green** 2-blink = alert delivered/acked; **red** 6-blink = urgent/undelivered. Wire:
  DATA-IN→D1/GPIO2, VDD→3V3, GND→GND.
- **Battery note (accepted):** the WS2812 draws ~0.6–1 mA even while dark, so on a 300 mAh
  cell TX life is ~**2 weeks** (LED-dominated), vs ~2 months if the LED were power-gated.
  User accepted the ~2-week figure; NOT adding a MOSFET high-side switch for now.

### 2026-07-10 change — TX "link down" rainbow indicator (⚠️ SUPERSEDED 2026-07-17 — rainbow removed, link-down is now the RED blink; see the 2026-07-17 entry above)
- **What:** when a HEARTBEAT is not app-ACKed by the RX (receiver unreachable), the TX now
  **stays awake and cycles a bright rainbow** (`RAINBOW_LEVEL 160`, ~2 s/sweep) on the D1
  pixel, re-sending the heartbeat each sweep, **until the RX acks**. Then it drops through
  the normal `goToSleep()` and resumes the standard 300 s cadence — no lingering state
  (the heartbeat/rainbow path never touches flash; only `seqCounter` advances).
- **Why:** a dead link should never happen in normal use, so the wearer must notice it
  immediately. Battery cost of staying awake is accepted by design (user's call).
- **Scope/priority:** an active ALERT still runs its own never-give-up **red 6-blink** loop
  and takes priority (that branch runs first); the rainbow is strictly the heartbeat/
  link-alive path, so the two never fight. Also fires on **cold-boot with the RX down**
  (power on the button while the base is off → immediate rainbow). Self-healing.
- **Verified on hardware 2026-07-10:** RX left off; at the next 5-min heartbeat the TX
  found the RX unreachable and began the continuous rainbow (confirms the timer-heartbeat
  path, not just cold-boot, triggers it). Recovery is self-healing by design — the next
  acked heartbeat drops through to normal `goToSleep()` (300 s cadence).

---

## 2. The two boards

| Role | Board | STA/base MAC | Current COM port | Notes |
|------|-------|--------------|------------------|-------|
| **RX** (receiver/alarm) | ESP32-WROOM ("spare") | `EC:E3:34:1A:64:FC` | COM3 | flashed `rx/`. **Promoted to RX 2026-07-12** (was the old WROOM TX / spare). `TX_MAC[]` unchanged (points at XIAO). |
| **TX** (button/transmitter) | XIAO ESP32-S3 | `E0:72:A1:F9:54:1C` | (vanishes on sleep; COM12 when awake) | flashed `tx/`; paired into `rx/`'s `TX_MAC[]` 2026-07-07. `tx/` `RX_MAC[]` retargeted `44:1D`→`EC:E3` on 2026-07-12. |

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
- **`HEARTBEAT_SECONDS = 300` in BOTH** (5 min, deployment value; must match). Was 20
  during testing. Offline threshold is now ~15 min (see §6/OFFLINE_MULT).
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
  sleep; the wake-cause check is `ESP_SLEEP_WAKEUP_EXT1`. Button is D9/GPIO8 to GND
  (moved from D0/GPIO1 on 2026-07-07). (Battery ADC note: ADC1 on the S3 is GPIO1-10.)
- **Both boards print their WiFi channel at boot** (`[tx]/[rx] wifi channel = 1`) — a
  leftover diagnostic from the board-swap hunt; handy to confirm both are on ch 1.
- Offline threshold = `HEARTBEAT_SECONDS * OFFLINE_MULT(3)` = ~900s (15 min) with the 300s heartbeat.

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
- **Behavioural consequence (intended):** a genuinely-triggered alert **cannot be cleared by
  power-cycling the TX** — only by a human pressing **clear at the RX** (which the RX signals
  back to the TX via the cleared ACK, 2026-07-17) or a reflash. (Pre-2026-07-17 the flag
  cleared on first delivery/ack; now it clears on the operator's clear — the TX stays awake
  and GREEN in between.)

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

**Resolved on the XIAO S3 (2026-07-06):** the trigger is now an external button on a
dedicated **RTC pad — D9/GPIO8** (EXT1 wake; moved from D0/GPIO1 on 2026-07-07), off any
strapping pin — so the prototype hazard above no longer applies. The XIAO's own BOOT button
is only used for entering the flash bootloader. (RX clear button can still be any GPIO.)

---

## 8. Next steps / roadmap

1. **Verify the low-battery paths on hardware** (still pending): the RX `LOW-BATTERY` state AND the
   new **TX blue blip / 10 s fast-wake** (2026-07-22). Test via `FAKE_BATTERY_MV 3200` +
   `BATTERY_SENSE_ENABLED 0` on the TX, or let a real cell sag below 3.4 V.
2. Wire peripherals: active buzzer (GPIO25), alarm LED (GPIO26), optional TX
   vibration motor (via transistor); verify the 3 warning states are audibly distinct.
   (RX external WS2812 rainbow/offline pixel on GPIO4 is **done + verified 2026-07-22**.)
3. ~~Battery divider on ADC1: set `BATTERY_SENSE_ENABLED 1`, calibrate, set `LOW_BATT_MV`.~~
   **DONE 2026-07-12** (divider on D2/GPIO3, `BATTERY_DIVIDER` 2.0306). Consider tuning the RX
   `LOW_BATT_MV` (3300) to align with the TX blue-blip threshold (3400) if you want them to match.
4. ~~Raise `HEARTBEAT_SECONDS` to 300 in **both** projects.~~ **DONE 2026-07-07** (offline now ~15 min).
5. Permanent build: ~~dedicated trigger GPIO (off BOOT)~~ **done on XIAO (D9/GPIO8)**;
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
