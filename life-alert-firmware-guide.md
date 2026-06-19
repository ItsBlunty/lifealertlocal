# Local Life-Alert — Firmware Implementation Guide

A two-device, in-home alert system built on ESP32-WROOM modules. A worn/mounted
**button (transmitter)** triggers a **persistent alarm in one room (receiver)**.
No internet, no router, no cloud, no medical service — everything stays inside the house.

> **This is not a certified medical device.** It is a DIY assistive tool. Do not
> let it be someone's only safety net. Test it thoroughly in the actual house
> before relying on it, and read the "Reliability requirements" and "Known
> limitations" sections — they are part of the spec, not footnotes.

---

## 1. Architecture

Two ESP32-WROOM devices talking over **ESP-NOW** (peer-to-peer, connectionless,
no WiFi router involved):

```
  [ TX: button ]                         [ RX: alarm ]
  battery powered                        mains powered
  deep sleep, wakes on:                  always awake
    - button press  --- ALERT --------->  latches alarm, sounds buzzer
    - heartbeat timer - HEARTBEAT ------>  tracks liveness + battery
                       <--- ACK ---------  confirms receipt
```

Why ESP-NOW and not WiFi: it removes the home router as a point of failure, has
effectively zero connection latency (no association handshake), and gives a
MAC-layer delivery callback we build confirmation on top of. Range through normal
interior walls easily covers a 1200 sqft house.

### Two firmware images
Build **two separate projects** (`tx/` and `rx/`), each with its own copy of the
shared `common.h`. Do not try to make one image do both roles.

---

## 2. Reliability requirements (treat these as acceptance criteria)

These are the things that separate a toy from something trustworthy. The build is
**not done** until all of these hold:

1. **End-to-end confirmation.** The button must give the wearer positive feedback
   (LED blink, and a vibration motor if fitted) **only after** the receiver has
   acknowledged the alert at the application level — not just after the radio sent
   the packet.
2. **Distinct failure feedback.** If an alert is *not* acknowledged after the full
   retry budget, the button must produce a clearly different, urgent pattern so the
   wearer knows to seek help another way. Silence on failure is unacceptable.
3. **Latching alarm.** A momentary press creates a *persistent* alarm at the
   receiver that sounds until a human physically clears it at the receiver. A brief
   press nobody happens to hear must not be lost.
4. **Liveness watchdog.** The receiver must detect when it has stopped hearing
   heartbeats from the button (dead battery, out of range, fault) and raise a
   distinct "button offline" warning.
5. **Low-battery warning.** The button reports its battery voltage in every message;
   the receiver raises a distinct low-battery warning. A silently dead button is
   worse than no button.
6. **A reset must never fire the alarm.** Only a genuine button press triggers an
   ALERT. Power-on, brownout, and reset events send at most a heartbeat.

The three receiver warning states (ALARM / OFFLINE / LOW BATTERY) must be
**audibly distinguishable** from each other.

---

## 3. Hardware assumptions & pin map

Prototype uses ESP32-WROOM dev boards. The **BOOT button (GPIO0)** is reused as
the trigger on TX and as the alarm-clear button on RX for now (see §9 for why this
is temporary).

### Transmitter (TX)
| Function            | Pin        | Notes |
|---------------------|------------|-------|
| Button (trigger)    | GPIO0      | BOOT button; RTC-capable so it can wake from deep sleep |
| Confirm LED         | GPIO2      | onboard LED on many dev boards |
| Vibration motor     | (optional) | drive via transistor/MOSFET, not directly |
| Battery sense (ADC) | GPIO34     | **must be an ADC1 pin (32–39)**; via voltage divider |

### Receiver (RX)
| Function          | Pin    | Notes |
|-------------------|--------|-------|
| Buzzer            | GPIO25 | use an **active** buzzer (digitalWrite on/off); for a passive buzzer use `tone()` |
| Alarm LED         | GPIO26 | |
| Status LED        | GPIO2  | onboard LED |
| Clear button      | GPIO0  | BOOT button, `INPUT_PULLUP`, active-low |

### Critical hardware gotchas
- **ADC2 is unusable while WiFi/ESP-NOW is active.** Battery sense **must** be on an
  ADC1 pin (GPIO32–39). GPIO34 is input-only ADC1 — good.
- The ESP32 ADC is noisy and nonlinear. Average multiple samples and use
  `analogReadMilliVolts()` (built-in calibration) rather than raw `analogRead()`.
- The battery divider ratio must match your resistors. Set `BATTERY_DIVIDER` =
  `(R1 + R2) / R2`. Keep divider resistors high-value (e.g. 100k/100k) to limit
  drain, but not so high the ADC can't settle.

---

## 4. Build environment

PlatformIO with the Arduino framework. Suggested per-project `platformio.ini`:

```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
```

> **Arduino-ESP32 core version matters.** This guide targets **core 3.x**
> (ESP-IDF 5.x), which is current. The ESP-NOW callback signatures **changed**
> between 2.x and 3.x. If the installed core is 2.x, adapt the callbacks:
>
> | Callback | 3.x (this guide) | 2.x |
> |---|---|---|
> | recv | `void cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)` | `void cb(const uint8_t *mac, const uint8_t *data, int len)` |
> | sent | `void cb(const wifi_tx_info_t *info, esp_now_send_status_t status)` | `void cb(const uint8_t *mac, esp_now_send_status_t status)` |
>
> Compile early; let the compiler tell you which signature the installed core wants,
> and adjust. In the 2.x recv callback, `info->src_addr` becomes the `mac` argument.

---

## 5. Shared protocol — `common.h`

Identical copy in both projects.

```cpp
#pragma once
#include <stdint.h>

#define PROTO_VERSION   1
#define ESPNOW_CHANNEL  1   // 1–13. BOTH devices MUST use the same channel.

enum MsgType : uint8_t {
  MSG_ALERT     = 1,
  MSG_HEARTBEAT = 2,
  MSG_ACK       = 3,
};

typedef struct __attribute__((packed)) {
  uint8_t  version;     // = PROTO_VERSION
  uint8_t  type;        // MsgType
  uint8_t  device_id;   // which button (1 for now; room to add more later)
  uint8_t  _pad;
  uint32_t seq;         // monotonic sequence number
  uint16_t battery_mv;  // battery millivolts (0 if N/A)
  uint16_t _pad2;
} Message;
```

Fixed-size, packed, well under the 250-byte ESP-NOW payload limit.

---

## 6. Bootstrap: discover the MAC addresses

ESP-NOW peers are addressed by MAC. Before the devices can talk, you must hardcode
each one's MAC into the other.

Both sketches print their own MAC on boot:
```cpp
Serial.print("MY MAC: "); Serial.println(WiFi.macAddress());
```
Flash each board once, read its MAC from the serial monitor, then fill in
`RX_MAC[]` in the TX project and `TX_MAC[]` in the RX project. (Use the STA MAC,
which is what `WiFi.macAddress()` returns in `WIFI_STA` mode.)

---

## 7. Transmitter firmware (`tx/src/main.cpp`)

Deep-sleeps; wakes on button press (ALERT) or on the heartbeat timer (HEARTBEAT);
sends, waits for confirmation, gives feedback, sleeps again.

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include "common.h"

// ---------------- CONFIG ----------------
uint8_t RX_MAC[6] = {0,0,0,0,0,0};   // <-- FILL IN receiver MAC

#define BUTTON_GPIO        GPIO_NUM_0
#define LED_GPIO           2
#define BATTERY_ADC_PIN    34         // ADC1 only!
#define HEARTBEAT_SECONDS  300        // heartbeat every 5 min
#define ACK_TIMEOUT_MS     400        // wait for app-level ACK per attempt
#define MAX_SEND_ATTEMPTS  20         // persistence for an alert
#define BATTERY_DIVIDER    2.0f       // (R1+R2)/R2 — match your divider
#define DEVICE_ID          1
// ----------------------------------------

RTC_DATA_ATTR uint32_t seqCounter = 0;   // survives deep sleep

volatile bool macCbFired = false, macDelivered = false, appAcked = false;

void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  macDelivered = (status == ESP_NOW_SEND_SUCCESS);
  macCbFired = true;
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(Message)) return;
  Message m; memcpy(&m, data, sizeof(m));
  if (m.version == PROTO_VERSION && m.type == MSG_ACK) appAcked = true;
}

uint16_t readBatteryMv() {
  uint32_t acc = 0; const int N = 16;
  for (int i = 0; i < N; i++) { acc += analogReadMilliVolts(BATTERY_ADC_PIN); delay(2); }
  return (uint16_t)((acc / (float)N) * BATTERY_DIVIDER);
}

void blink(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_GPIO, HIGH); delay(onMs);
    digitalWrite(LED_GPIO, LOW);  delay(offMs);
  }
}

bool sendMessage(const Message &msg) {
  for (int attempt = 0; attempt < MAX_SEND_ATTEMPTS; attempt++) {
    macCbFired = macDelivered = appAcked = false;
    if (esp_now_send(RX_MAC, (const uint8_t*)&msg, sizeof(msg)) != ESP_OK) {
      delay(50); continue;
    }
    uint32_t t0 = millis();
    while (!macCbFired && millis() - t0 < 100) delay(1);   // MAC-layer result
    if (macDelivered) {
      uint32_t t1 = millis();
      while (!appAcked && millis() - t1 < ACK_TIMEOUT_MS) delay(1);  // app ACK
      if (appAcked) return true;
    }
    delay(50);   // backoff before retry
  }
  return false;
}

void goToSleep() {
  esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);   // 0 = wake when GPIO0 goes LOW
  esp_sleep_enable_timer_wakeup((uint64_t)HEARTBEAT_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(LED_GPIO, OUTPUT); digitalWrite(LED_GPIO, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) goToSleep();
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RX_MAC, 6);
  peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

  Message msg = {};
  msg.version = PROTO_VERSION;
  msg.device_id = DEVICE_ID;
  msg.seq = ++seqCounter;
  msg.battery_mv = readBatteryMv();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // REAL BUTTON PRESS -> alert
    msg.type = MSG_ALERT;
    bool ok = sendMessage(msg);
    if (ok) blink(2, 60, 80);        // confirmed: two quick blinks
    else    blink(6, 300, 150);      // FAILED: long urgent pattern
  } else {
    // timer heartbeat, OR cold boot / reset / brownout -> heartbeat only.
    // A reset must NEVER fire the alarm (requirement #6).
    msg.type = MSG_HEARTBEAT;
    sendMessage(msg);                // best effort
  }

  goToSleep();
}

void loop() {}   // unreached: all logic runs in setup() before deep sleep
```

Notes for the implementer:
- `RTC_DATA_ATTR` keeps `seqCounter` across deep sleep.
- For deep-sleep wake on GPIO0, the line must be held high while asleep. Dev boards
  have an external pull-up on the BOOT line, so this works as-is on a dev board. On a
  bare module, add a pull-up (and optionally `rtc_gpio_pullup_en`).
- If a vibration motor is fitted, drive it alongside the confirm/failure blinks via a
  transistor — never straight off a GPIO.

---

## 8. Receiver firmware (`rx/src/main.cpp`)

Always awake. Latches the alarm on ALERT, tracks liveness/battery from heartbeats,
and drives three distinguishable warning states plus an idle state.

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "common.h"

// ---------------- CONFIG ----------------
uint8_t TX_MAC[6] = {0,0,0,0,0,0};   // <-- FILL IN transmitter MAC

#define BUZZER_GPIO        25         // active buzzer
#define ALARM_LED_GPIO     26
#define STATUS_LED_GPIO    2
#define CLEAR_BUTTON_GPIO  0          // BOOT button clears alarm (prototype)
#define HEARTBEAT_SECONDS  300        // must match TX
#define OFFLINE_MULT       3          // offline after 3 missed heartbeats
#define LOW_BATT_MV        3300       // tune to battery chemistry
// ----------------------------------------

volatile bool     alarmLatched = false;
volatile uint32_t lastHeartbeatMs = 0;
volatile uint16_t lastBatteryMv = 0;
volatile bool     batteryLow = false;

void sendAck(const uint8_t *dst, uint32_t seq) {
  Message a = {}; a.version = PROTO_VERSION; a.type = MSG_ACK; a.seq = seq;
  esp_now_send(dst, (const uint8_t*)&a, sizeof(a));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(Message)) return;
  Message m; memcpy(&m, data, sizeof(m));
  if (m.version != PROTO_VERSION) return;

  lastHeartbeatMs = millis();
  if (m.battery_mv) { lastBatteryMv = m.battery_mv; batteryLow = (m.battery_mv < LOW_BATT_MV); }

  if (m.type == MSG_ALERT)     { alarmLatched = true; sendAck(info->src_addr, m.seq); }
  else if (m.type == MSG_HEARTBEAT) {                 sendAck(info->src_addr, m.seq); }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_GPIO, OUTPUT);
  pinMode(ALARM_LED_GPIO, OUTPUT);
  pinMode(STATUS_LED_GPIO, OUTPUT);
  pinMode(CLEAR_BUTTON_GPIO, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};       // add TX so we can ACK back
  memcpy(peer.peer_addr, TX_MAC, 6);
  peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

  lastHeartbeatMs = millis();          // grace period at startup
  Serial.print("RX MAC: "); Serial.println(WiFi.macAddress());
}

// --- distinguishable indicator patterns (non-blocking, millis-based) ---
void buzz(bool on) { digitalWrite(BUZZER_GPIO, on ? HIGH : LOW); }

void loop() {
  uint32_t now = millis();

  // clear button (active low, debounced)
  static uint32_t lastPress = 0;
  if (digitalRead(CLEAR_BUTTON_GPIO) == LOW && now - lastPress > 300) {
    lastPress = now;
    alarmLatched = false;
  }

  bool offline = (now - lastHeartbeatMs) >
                 (uint32_t)HEARTBEAT_SECONDS * OFFLINE_MULT * 1000UL;

  if (alarmLatched) {
    // ALARM: urgent — fast on/off, alarm LED on
    digitalWrite(ALARM_LED_GPIO, HIGH);
    buzz((now / 250) % 2);                 // ~2 Hz continuous
  } else if (offline) {
    // OFFLINE: double-chirp every ~8 s
    digitalWrite(ALARM_LED_GPIO, LOW);
    uint32_t p = now % 8000;
    buzz(p < 80 || (p > 200 && p < 280));
  } else if (batteryLow) {
    // LOW BATTERY: single short chirp every ~30 s
    digitalWrite(ALARM_LED_GPIO, LOW);
    buzz((now % 30000) < 80);
  } else {
    // IDLE: silent, status LED brief blink every 3 s
    digitalWrite(ALARM_LED_GPIO, LOW);
    buzz(false);
    digitalWrite(STATUS_LED_GPIO, (now % 3000) < 40);
  }
}
```

Notes for the implementer:
- Keep the loop non-blocking (no `delay()` in patterns) so the clear button and
  incoming packets stay responsive while the buzzer is sounding.
- For a **passive** buzzer, replace `buzz()` with `tone(BUZZER_GPIO, 2500)` /
  `noTone(BUZZER_GPIO)`. An active buzzer is simpler — recommend it for the prototype.
- Sequence-number dedup isn't required because the alarm latches (a duplicate ALERT
  is harmless), but logging `seq` to serial helps debugging.

---

## 9. Indicator reference

**Transmitter (button) feedback**
| Event | Pattern |
|---|---|
| Alert confirmed (ACK received) | 2 quick blinks (+ vibrate) |
| Alert FAILED (no ACK after all retries) | 6 long urgent blinks — *seek help another way* |
| Heartbeat | no user-facing indication |

**Receiver states** (must be audibly distinct)
| State | Buzzer | LED |
|---|---|---|
| Idle | silent | status LED brief blink / 3 s |
| ALARM (latched) | ~2 Hz continuous | alarm LED solid |
| Button OFFLINE | double-chirp / 8 s | — |
| Low battery | single chirp / 30 s | — |

---

## 10. Test plan / acceptance

1. **MAC bootstrap** — both boards print their MAC; addresses filled in correctly.
2. **Same channel** — `ESPNOW_CHANNEL` identical in both `common.h` copies.
3. **Happy path** — press button → RX alarm latches and sounds → TX gives 2-blink
   confirm. Latency should feel instant (<0.5 s).
4. **Latching** — alarm keeps sounding after the button is released; only the RX
   clear button stops it.
5. **Failure feedback** — power the RX off, press the button → TX gives the long
   failure pattern (no false confirm).
6. **Reset safety** — reset the TX (EN button / power cycle) with the BOOT button
   *held down* → the alarm must NOT fire. (Note the strapping-pin caveat in §11.)
7. **Offline detection** — remove TX power; within ~3× heartbeat interval the RX
   shows the offline pattern. Shorten `HEARTBEAT_SECONDS` to ~20 s during testing.
8. **Low battery** — temporarily set `LOW_BATT_MV` high so a healthy pack reads
   "low"; confirm the low-battery pattern, distinct from offline.
9. **Range** — walk the button to the far corners of the actual house, behind closed
   doors, and confirm reliable delivery in every room a person might be in.

---

## 11. Known limitations & reliability caveats

- **Not a certified medical/safety device.** No regulatory testing, no guaranteed
  uptime. It must not be someone's only line of help.
- **BOOT button is a strapping pin (temporary only).** If the TX resets/browns-out
  *while GPIO0 is held low*, it boots into download mode and won't run. Fine for
  bench testing; for the permanent worn unit, move the trigger to a plain GPIO with
  an external button and update `BUTTON_GPIO` (it must still be an RTC GPIO to wake
  from deep sleep — e.g. GPIO33). Same applies to the RX clear button (any GPIO is
  fine there).
- **No encryption.** ESP-NOW supports PMK/LMK encryption; not enabled here for
  simplicity. Add it if you care about a neighbor spoofing alerts. It's local-only
  RF either way.
- **Single button, single receiver.** `device_id` leaves room to scale, but the
  current code pairs one TX to one RX.
- **ADC accuracy.** Battery voltage is approximate; calibrate `BATTERY_DIVIDER`
  against a multimeter and set `LOW_BATT_MV` with margin above the battery's cutoff.
- **Mains-only receiver.** If household power drops, the alarm is dead. Consider a
  small UPS/battery backup on the RX for the real build.

---

## 12. Roadmap to the permanent build (out of scope for now)

- Replace BOOT-button trigger with a dedicated external button on an RTC GPIO.
- Bare/low-power module or a board with low deep-sleep current; clean LDO. Target
  months-to-years on the TX battery.
- Optional vibration motor on TX for tactile confirmation.
- Enclosure for worn unit (sweat/water resistance, comfortable button force).
- Optional ESP-NOW encryption.
- Optional RX power backup.
