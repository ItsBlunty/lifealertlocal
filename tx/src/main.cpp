// Local Life-Alert — TRANSMITTER (worn/mounted button)
//
// Target board: Seeed XIAO ESP32-S3.
//
// Deep-sleeps; wakes on a button press (-> ALERT) or the heartbeat timer
// (-> HEARTBEAT); sends, waits for an APP-LEVEL ack, gives feedback, sleeps.
// A bare reset / power-on / brownout must NEVER fire a NEW alarm (requirement #6): the
// alert flag is only ever set by a debounced EXT1 button press, so a spurious reset alone
// never creates one. But once a real press sets it, the flag is stored in FLASH (NVS) and
// the alert is re-sent after ANY reset -- brownout, watchdog, the EN/RESET button, even a
// full power-off / dead battery. While the alert is active the TX stays awake and blinks
// GREEN (RX reachable/latched) or RED (RX unreachable); it only stands down once a human
// operator CLEARS the alarm at the RX (signalled back in the ACK). A genuinely triggered
// alert is therefore never lost; it clears only on an operator clear (or a reflash).
// NOTE: the S3 has no EXT0 peripheral (unlike the classic ESP32-WROOM), so the
// button wake uses EXT1 (ANY_LOW) instead of ext0.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include "driver/rtc_io.h"
#include "common.h"

// ---------------- CONFIG ----------------
uint8_t RX_MAC[6] = {0xEC, 0xE3, 0x34, 0x1A, 0x64, 0xFC};   // receiver = WROOM "spare" EC:E3, promoted to RX 2026-07-12 (was 44:1D:64:F5:87:F8)

#define BUTTON_GPIO         GPIO_NUM_8   // XIAO ESP32-S3 pad D9 (RTC-capable; EXT1 wake). External button to GND.
// Addressable RGB LED (WS2812/NeoPixel-compatible) on XIAO pad D1 = GPIO2 — replaces the
// onboard LED for all confirm/failure feedback (green = delivered, red = urgent/undelivered).
// Driven by the Arduino-ESP32 core's built-in RMT helper neopixelWrite(), so NO external
// library / lib_deps entry is needed. DATA-IN -> GPIO2; VDD -> 3V3 (or 5V), GND -> GND.
// BATTERY WARNING: a WS2812 draws ~0.6-1 mA even while showing black, which DWARFS the ESP32
// deep-sleep current (~0.05 mA). Left always-powered it dominates drain (~2 wk on a 300 mAh
// cell). For battery use, gate the pixel's VDD with a high-side P-MOSFET off a spare GPIO so
// it's fully unpowered during deep sleep (see HANDOFF roadmap).
#define RGBLED_GPIO         2            // XIAO pad D1 — WS2812 DATA-IN
#define RGB_LEVEL           60           // per-channel brightness (0-255); modest to limit current
// Two-state feedback (plus off). GREEN repeated blink = alert delivered & acked ("someone is
// coming"); RED repeated blink = something is wrong (alert not acked, OR link down). Same
// cadence for both so the wearer only has to learn the color, not the pattern.
#define BLINK_ON_MS         250          // blink on-time
#define BLINK_OFF_MS        250          // blink off-time
#define BLINK_BURST         6            // blinks per round while alerting (green=coming / red=wrong)
#define GREEN_ESCORT_BLINKS 50           // after the RX operator clears: reassurance blinks for the responder's trip
#define HEARTBEAT_SECONDS   300          // deployment value (5 min). MUST match RX.
#define ACK_TIMEOUT_MS      400          // wait for app-level ACK per attempt
#define MAX_SEND_ATTEMPTS   20           // persistence for an alert
#define DEVICE_ID           1
// Low-battery indicator (LOWEST-priority LED mode): while the cell is below LOW_BATT_MV the TX
// blips a single BLUE flash and then deep-sleeps only LOW_BATT_SLEEP_SEC (not HEARTBEAT_SECONDS),
// so you see ~one blue flash every 10 s. It lives only in the link-healthy/idle path, so GREEN
// (alerting) and RED (link down/undelivered) always override it — anything but Off wins.
#define LOW_BATT_MV         3400         // TX-local threshold; independent of the RX's own LOW_BATT_MV
#define LOW_BATT_SLEEP_SEC  10           // fast wake cadence while low (blue blip + heartbeat), vs 300 s normal

// Button debounce / anti-wedge (see the "stuck-awake heartbeat flood" note in HANDOFF).
// Biased HARD toward CATCHING a real press (a life-alert must not drop one): DEBOUNCE_MS is
// the minimum continuous-LOW time that counts as a press — lowered 40->10 (~5 poll cycles at
// the 2 ms read interval) so even a quick/marginal press latches. It still rejects a single
// stray sample, but noise rejection is intentionally minimal; RAISE it if real-world noise
// causes false alerts. PRESS_CONFIRM_MS is the WINDOW we keep looking for that stable-LOW
// after an EXT1 wake (a timeout, NOT a hold time); widened 120->300 so a bouncy contact that
// settles late still confirms. It early-exits the moment a real press is seen, so the wider
// window only costs a little extra awake time on a false EXT1 glitch. See HANDOFF
// §2026-07-12 (marginal D9 press -> heartbeat).
#define DEBOUNCE_MS         10           // min continuous-LOW to accept as a press (was 40; lower=catch more, less noise reject)
#define PRESS_CONFIRM_MS    300          // window to find a debounced press after an EXT1 wake (was 120)
#define RELEASE_WAIT_MS     6000         // max wait for a debounced release before sleeping (timer-only)

// Battery sense is OFF until a divider is wired (guide §3).
//   BATTERY_SENSE_ENABLED 0 -> report FAKE_BATTERY_MV.
//        FAKE_BATTERY_MV 0    -> "N/A": RX never raises a (false) low-battery warning.
//        FAKE_BATTERY_MV 3200 -> exercise the RX low-battery path end-to-end (it's < LOW_BATT_MV).
//   BATTERY_SENSE_ENABLED 1 -> read the real divider on BATTERY_ADC_PIN (ADC1 on S3 = GPIO1-10; the XIAO
//        has no built-in battery divider, so wire your own to a free pad, e.g. D2/GPIO3).
#define BATTERY_SENSE_ENABLED  1
#define FAKE_BATTERY_MV        0
#define BATTERY_ADC_PIN        3
#define BATTERY_DIVIDER        2.0306f   // calibrated 2026-07-12: meter 3.85V vs reported 3792mV (2.0*3850/3792)
// ----------------------------------------

// Heartbeat sequence lives in RTC NOINIT (survives deep sleep + brownout; the magic guard
// re-inits it on a true power-on). It's non-critical, so it doesn't need flash.
#define RTC_MAGIC 0x2712A11E
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t seqCounter;

// The undelivered-alert flag lives in FLASH (NVS), not RTC, so it survives EVERYTHING --
// brownout, watchdog, the EN/RESET button, and even a full power-off / dead battery. We
// write it only on a real debounced press and on delivery (~2 flash writes per alert), so
// wear is a non-issue. NVS defaults to false on a fresh chip, so no magic guard is needed.
Preferences prefs;
bool     alertPending = false;   // mirror of the flash flag, loaded each boot
uint32_t alertSeq     = 0;       // fixed seq for the pending alert (RX latch is idempotent)
bool     lowBattery   = false;   // set on the idle/healthy path -> goToSleep() uses the fast wake cadence

void loadAlertState() {
  prefs.begin("lifealert", true);            // read-only
  alertPending = prefs.getBool("pending", false);
  alertSeq     = prefs.getUInt("aseq", 0);
  prefs.end();
}
void saveAlertState() {
  prefs.begin("lifealert", false);           // read-write
  prefs.putBool("pending", alertPending);
  prefs.putUInt("aseq", alertSeq);
  prefs.end();
}

// Latch a NEW alert: set the pending flag + a fresh FIXED seq and PERSIST to flash NOW,
// before any send, so even a brownout/power-loss during the first transmit can't lose it.
// Called both on a debounced wake-press and when a press is POLLED during a link-down loop.
void latchAlert() {
  alertPending = true;
  alertSeq     = ++seqCounter;               // one seq for this alert; reused across retries/resets
  saveAlertState();
}

volatile bool macCbFired = false, macDelivered = false, appAcked = false;
volatile bool ackAlarmActive = false;   // last ACK's alarm state: true = RX still latched, false = operator cleared

// core >= 3.3.0 send-callback signature (wifi_tx_info_t).
// On core 3.0-3.2 this would be (const uint8_t *mac, ...); on 2.x likewise.
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  macDelivered = (status == ESP_NOW_SEND_SUCCESS);
  macCbFired = true;
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(Message)) return;
  Message m; memcpy(&m, data, sizeof(m));
  if (m.version == PROTO_VERSION && m.type == MSG_ACK) {
    ackAlarmActive = (m.flags & ACK_ALARM_ACTIVE) != 0;   // valid to read once appAcked is set
    appAcked = true;
  }
}

uint16_t readBatteryMv() {
#if BATTERY_SENSE_ENABLED
  uint32_t acc = 0; const int N = 16;
  for (int i = 0; i < N; i++) { acc += analogReadMilliVolts(BATTERY_ADC_PIN); delay(2); }
  return (uint16_t)((acc / (float)N) * BATTERY_DIVIDER);
#else
  return FAKE_BATTERY_MV;
#endif
}

// Addressable-LED helpers. neopixelWrite(pin, R, G, B) is provided by the core
// (esp32-hal-rgb-led) and drives one WS2812 pixel via RMT; it configures the pin itself,
// so no pinMode() is needed.
void ledOff() { neopixelWrite(RGBLED_GPIO, 0, 0, 0); }
void ledColor(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(RGBLED_GPIO, r, g, b); }

// Blink the pixel `times` in the given color (leaves it off afterward).
void blinkColor(int times, int onMs, int offMs, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < times; i++) {
    ledColor(r, g, b); delay(onMs);
    ledOff();          delay(offMs);
  }
}

// Blink RED and GREEN helpers — the only two feedback states (plus off).
// RED = something is wrong (RX unreachable); GREEN = someone is coming (alert received/active).
void blinkRed(int n = BLINK_BURST)   { blinkColor(n, BLINK_ON_MS, BLINK_OFF_MS, RGB_LEVEL, 0, 0); }
void blinkGreen(int n = BLINK_BURST) { blinkColor(n, BLINK_ON_MS, BLINK_OFF_MS, 0, RGB_LEVEL, 0); }
// BLUE = lowest-priority low-battery blip (one flash per idle wake). See LOW_BATT_MV.
void blinkBlue(int n = 1)            { blinkColor(n, BLINK_ON_MS, BLINK_OFF_MS, 0, 0, RGB_LEVEL); }

// ---- boot-only battery-VOLTAGE readout on the RGB pixel (diagnostic) ----
// Runs ONCE on a true power-on / RESET (not on a deep-sleep wake) and NOT while an alert is
// pending (an alert must show immediately, never wait behind a diagnostic). Sequence:
//   * 5 PURPLE flashes                = "battery readout coming"
//   * then 3 identical rounds of:  ORANGE x (volts ones digit), then BLUE x (tenths digit)
//     e.g. 3.8 V -> 3 orange then 8 blue, repeated 3x  (voltage rounded to the nearest 0.1 V).
// Flashes are ~2/s so they're countable; the whole readout takes ~15-20 s.
// CAVEAT: a digit of 0 (e.g. exactly 4.0 V, or "N/A" when battery sense is off) shows as NO
// flashes for that digit — 4.0 V reads as "4 orange, then nothing".
#define READOUT_ON_MS   220
#define READOUT_OFF_MS  200
void showBatteryLevel() {
  uint16_t mv     = readBatteryMv();
  uint16_t tenths = (mv + 50) / 100;          // round to nearest 0.1 V: 3830 -> 38
  uint8_t  ones   = tenths / 10;              // volts ones digit (3.x -> 3)
  uint8_t  dec    = tenths % 10;              // tenths digit     (x.8 -> 8)
  Serial.printf("[tx] boot battery readout: %u mV -> %u.%u V  (%u orange + %u blue, x3)\n",
                mv, ones, dec, ones, dec);
  blinkColor(5, READOUT_ON_MS, READOUT_OFF_MS, RGB_LEVEL, 0, RGB_LEVEL);   // 5 PURPLE = readout coming
  delay(600);
  for (int round = 0; round < 3; round++) {
    blinkColor(ones, READOUT_ON_MS, READOUT_OFF_MS, RGB_LEVEL, RGB_LEVEL * 2 / 5, 0);  // ORANGE x ones
    delay(500);                                                                        // gap: ones -> tenths
    blinkColor(dec,  READOUT_ON_MS, READOUT_OFF_MS, 0, 0, RGB_LEVEL);                   // BLUE   x tenths
    delay(900);                                                                        // gap between rounds
  }
  ledOff();
}

// Blink one RED round (~BLINK_BURST on/off cycles) while CONTINUOUSLY watching the button.
// Returns true the instant a debounced press (>= DEBOUNCE_MS continuous LOW) is seen, so a
// wearer who presses during an awake link-down loop still gets a real ALERT. EXT1 only wakes
// the chip from deep sleep — while we're awake the button must be POLLED, not waited-on via
// wake. Leaves the pixel off. (The digital driver already owns the pad via prepareButtonInput.)
bool blinkRedWatchButton() {
  for (int seg = 0; seg < BLINK_BURST * 2; seg++) {
    bool on = (seg % 2 == 0);
    ledColor(on ? RGB_LEVEL : 0, 0, 0);
    uint32_t dur = on ? BLINK_ON_MS : BLINK_OFF_MS;
    uint32_t start = millis(), lowSince = 0;
    bool low = false;
    while (millis() - start < dur) {
      if (digitalRead(BUTTON_GPIO) == LOW) {
        if (!low) { low = true; lowSince = millis(); }
        else if (millis() - lowSince >= DEBOUNCE_MS) { ledOff(); return true; }
      } else {
        low = false;
      }
      delay(2);
    }
  }
  ledOff();
  return false;
}

// Returns true only after the RECEIVER app-level ACK is received (requirement #1).
bool sendMessage(const Message &msg) {
  for (int attempt = 0; attempt < MAX_SEND_ATTEMPTS; attempt++) {
    macCbFired = macDelivered = appAcked = ackAlarmActive = false;
    if (esp_now_send(RX_MAC, (const uint8_t *)&msg, sizeof(msg)) != ESP_OK) {
      delay(50); continue;
    }
    uint32_t t0 = millis();
    while (!macCbFired && millis() - t0 < 100) delay(1);             // MAC-layer result
    if (macDelivered) {
      uint32_t t1 = millis();
      while (!appAcked && millis() - t1 < ACK_TIMEOUT_MS) delay(1);  // app-level ACK
      if (appAcked) return true;
    }
    delay(50);                                                       // backoff before retry
  }
  return false;
}

// Give the digital driver the button pad (release any RTC hold from a prior sleep)
// so we can read/debounce it. Idempotent; safe on cold boot too.
void prepareButtonInput() {
  rtc_gpio_deinit(BUTTON_GPIO);
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
}

// Wait until the button reads `level` (LOW/HIGH) continuously for DEBOUNCE_MS, or
// until timeoutMs elapses. Returns true only if the stable level was reached.
bool waitButtonStable(int level, uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t stableSince = start;
  while (millis() - start < timeoutMs) {
    if (digitalRead(BUTTON_GPIO) == level) {
      if (millis() - stableSince >= DEBOUNCE_MS) return true;
    } else {
      stableSince = millis();
    }
    delay(2);
  }
  return false;
}

void goToSleep() {
  Serial.flush();
  // S3 has no EXT0: wake on the button via EXT1 (ANY_LOW). ONLY arm it when the pad
  // is actually released (HIGH). Arming wake-on-LOW while the pad is still LOW makes
  // deep sleep wake immediately -> the stuck-awake churn/flood. If it's still held,
  // sleep on the heartbeat TIMER only and re-arm the button on a later (released) wake.
  if (digitalRead(BUTTON_GPIO) == HIGH) {
    rtc_gpio_pullup_en(BUTTON_GPIO);
    rtc_gpio_pulldown_dis(BUTTON_GPIO);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
  } else {
    Serial.println("button still LOW at sleep -> timer-only wake (will re-arm once released)");
  }
  // While the battery is low, wake often (fast blue-blip cadence); otherwise the normal 5-min heartbeat.
  uint32_t sleepSec = lowBattery ? LOW_BATT_SLEEP_SEC : HEARTBEAT_SECONDS;
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  ledOff();

  // Re-init the (non-critical) heartbeat counter only on a true power-on, when RTC RAM is
  // garbage. The alert flag is NOT here -- it lives in flash and is loaded next.
  if (rtcMagic != RTC_MAGIC) {
    rtcMagic   = RTC_MAGIC;
    seqCounter = 0;
  }
  loadAlertState();   // pull the undelivered-alert flag from flash (survives any reset)

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // Boot-only battery-voltage readout: only on a true RESET / power-on (UNDEFINED wake cause,
  // i.e. NOT a deep-sleep timer/EXT1 wake), and skipped while an alert is pending so a reset
  // mid-alert resumes the alert immediately instead of waiting behind the ~15-20 s diagnostic.
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED && !alertPending) showBatteryLevel();

  WiFi.mode(WIFI_STA);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  { uint8_t pc; wifi_second_chan_t sc; esp_wifi_get_channel(&pc, &sc);
    Serial.printf("[tx] wifi channel = %u (want %u)\n", pc, ESPNOW_CHANNEL); }
  if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init failed; sleeping"); goToSleep(); }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  // Read the MAC only AFTER esp_now_init(); reading right after WiFi.mode()
  // returns 00:00:00:00:00:00 because the WiFi driver isn't up yet.
  Serial.print("TX MAC (put this in TX_MAC[] of rx/src/main.cpp): ");
  Serial.println(WiFi.macAddress());

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RX_MAC, 6);
  peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

  prepareButtonInput();   // own the pad so we can debounce it (and so goToSleep can read it)

  // A button (EXT1) wake only becomes an ALERT if it survives debounce — a sustained,
  // genuine press. Contact-bounce glitches that trip EXT1 without a real press fall
  // through to a harmless heartbeat (requirement #6: never alarm on noise), and no
  // longer leave the pad half-pressed into the stuck-awake wedge. A confirmed press
  // latches alertPending in FLASH so the alert survives any reset until delivered.
  bool newPress = false;
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    newPress = waitButtonStable(LOW, PRESS_CONFIRM_MS);
    if (!newPress) Serial.println("EXT1 wake but no debounced press (glitch) -> heartbeat");
  }
  if (newPress) latchAlert();        // debounced wake-press -> persisted alert (survives any reset)

  Message msg = {};
  msg.version   = PROTO_VERSION;
  msg.device_id = DEVICE_ID;

  if (alertPending)
    Serial.printf("ALERT seq=%lu — staying awake, GREEN until the RX operator clears (persists across resets)\n",
                  (unsigned long)alertSeq);

  // Unified awake loop. While an ALERT is pending we keep (re)sending it — GREEN when the RX
  // has it latched ("someone is coming"), RED when the RX is unreachable ("something is
  // wrong") — never sleeping until the operator CLEARS it (signalled back in the ACK). That
  // lets the responder stop the TX draining its battery, and the FIXED, flash-persisted seq
  // means a reset/brownout mid-alert resumes right here (req #6: a bare reset never clears it).
  // Otherwise we send a HEARTBEAT; if the link is down we blink RED **and POLL the button**,
  // so a press during the awake link-down loop escalates into a real, persisted ALERT (EXT1
  // only wakes from sleep, so while awake the button must be polled — this was the dropped-
  // press bug). Self-healing: the moment the RX returns, the next heartbeat acks and we sleep.
  for (;;) {
    if (alertPending) {
      msg.type       = MSG_ALERT;
      msg.seq        = alertSeq;
      msg.battery_mv = readBatteryMv();          // refresh so the RX sees live battery
      bool acked = sendMessage(msg);
      if (!acked) {
        Serial.printf("[tx] ALERT seq=%lu — RX UNREACHABLE (red), will keep trying\n", (unsigned long)alertSeq);
        blinkRed();                              // RED = something is wrong: seek help another way
      } else if (ackAlarmActive) {
        blinkGreen();                            // GREEN = someone is coming: RX has the alarm latched
      } else {
        // operator cleared it -> escort the responder in with GREEN, then drop to normal cycle
        Serial.println("[tx] RX operator CLEARED the alarm — GREEN escort blinks, then normal cycle");
        blinkGreen(GREEN_ESCORT_BLINKS);
        alertPending = false;
        saveAlertState();                        // clear persisted flag so we don't resume after reboot
        waitButtonStable(HIGH, RELEASE_WAIT_MS); // debounced release so bounce can't re-arm EXT1
        break;
      }
    } else {
      msg.type       = MSG_HEARTBEAT;
      msg.seq        = ++seqCounter;
      msg.battery_mv = readBatteryMv();
      Serial.printf("HEARTBEAT seq=%lu batt=%u (wakeup_cause=%d) ... ",
                    (unsigned long)msg.seq, msg.battery_mv, (int)cause);
      bool ok = sendMessage(msg);
      Serial.println(ok ? "ack" : "no ack");
      if (ok) {
        // Link healthy, no alert -> the ONLY place the low-priority blue blip runs (0 mV = N/A).
        lowBattery = (msg.battery_mv != 0 && msg.battery_mv < LOW_BATT_MV);
        if (lowBattery) {
          Serial.printf("[tx] battery LOW (%u mV < %u) -> blue blip, wake every %ds\n",
                        msg.battery_mv, LOW_BATT_MV, LOW_BATT_SLEEP_SEC);
          blinkBlue();
        }
        break;                                   // sleep (fast cadence if low, else 300 s)
      }

      // LINK DOWN: RX unreachable. Blink RED while WATCHING the button; a press escalates to a
      // real persisted ALERT (the old loop never read the button, so a press here was lost).
      if (blinkRedWatchButton()) {
        latchAlert();
        Serial.printf("[tx] button pressed during link-down -> ALERT seq=%lu (persisted)\n",
                      (unsigned long)alertSeq);
        // loop: alertPending is now true -> handled by the ALERT branch above
      }
      // else keep looping as a heartbeat link-down (RED)
    }
  }

  goToSleep();
}

void loop() {}   // unreached: all logic runs in setup() before deep sleep
