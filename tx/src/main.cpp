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
// full power-off / dead battery -- until the RX confirms the latch. A genuinely triggered
// alert is therefore never lost; it clears only on delivery (or a reflash).
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
uint8_t RX_MAC[6] = {0x44, 0x1D, 0x64, 0xF5, 0x87, 0xF8};   // receiver (board #1)

#define BUTTON_GPIO         GPIO_NUM_1   // XIAO ESP32-S3 pad D0 (RTC-capable; EXT1 wake). External button to GND.
#define LED_GPIO            21           // XIAO ESP32-S3 onboard user LED (ACTIVE-LOW) = confirm / failure feedback
#define LED_ACTIVE_LOW      1            // onboard LED sinks current: LOW = on
#define HEARTBEAT_SECONDS   20           // TESTING value. TODO: 300 (5 min) for deployment. MUST match RX.
#define ACK_TIMEOUT_MS      400          // wait for app-level ACK per attempt
#define MAX_SEND_ATTEMPTS   20           // persistence for an alert
#define DEVICE_ID           1

// Button debounce / anti-wedge (see the "stuck-awake heartbeat flood" note in HANDOFF).
#define DEBOUNCE_MS         40           // pad level must hold this long to be accepted
#define PRESS_CONFIRM_MS    120          // window to confirm a debounced press after an EXT1 wake
#define RELEASE_WAIT_MS     6000         // max wait for a debounced release before sleeping (timer-only)

// Battery sense is OFF until a divider is wired (guide §3).
//   BATTERY_SENSE_ENABLED 0 -> report FAKE_BATTERY_MV.
//        FAKE_BATTERY_MV 0    -> "N/A": RX never raises a (false) low-battery warning.
//        FAKE_BATTERY_MV 3200 -> exercise the RX low-battery path end-to-end (it's < LOW_BATT_MV).
//   BATTERY_SENSE_ENABLED 1 -> read the real divider on BATTERY_ADC_PIN (ADC1 on S3 = GPIO1-10; the XIAO
//        has no built-in battery divider, so wire your own to a free pad, e.g. D2/GPIO3).
#define BATTERY_SENSE_ENABLED  0
#define FAKE_BATTERY_MV        0
#define BATTERY_ADC_PIN        3
#define BATTERY_DIVIDER        2.0f      // (R1+R2)/R2 — match your divider, then calibrate vs a multimeter
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

volatile bool macCbFired = false, macDelivered = false, appAcked = false;

// core >= 3.3.0 send-callback signature (wifi_tx_info_t).
// On core 3.0-3.2 this would be (const uint8_t *mac, ...); on 2.x likewise.
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
#if BATTERY_SENSE_ENABLED
  uint32_t acc = 0; const int N = 16;
  for (int i = 0; i < N; i++) { acc += analogReadMilliVolts(BATTERY_ADC_PIN); delay(2); }
  return (uint16_t)((acc / (float)N) * BATTERY_DIVIDER);
#else
  return FAKE_BATTERY_MV;
#endif
}

// Onboard LED is active-low on the XIAO S3; wrap it so the logic reads "on/off".
void ledSet(bool on) {
#if LED_ACTIVE_LOW
  digitalWrite(LED_GPIO, on ? LOW : HIGH);
#else
  digitalWrite(LED_GPIO, on ? HIGH : LOW);
#endif
}

void blink(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    ledSet(true);  delay(onMs);
    ledSet(false); delay(offMs);
  }
}

// Returns true only after the RECEIVER app-level ACK is received (requirement #1).
bool sendMessage(const Message &msg) {
  for (int attempt = 0; attempt < MAX_SEND_ATTEMPTS; attempt++) {
    macCbFired = macDelivered = appAcked = false;
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
  esp_sleep_enable_timer_wakeup((uint64_t)HEARTBEAT_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(LED_GPIO, OUTPUT); ledSet(false);

  // Re-init the (non-critical) heartbeat counter only on a true power-on, when RTC RAM is
  // garbage. The alert flag is NOT here -- it lives in flash and is loaded next.
  if (rtcMagic != RTC_MAGIC) {
    rtcMagic   = RTC_MAGIC;
    seqCounter = 0;
  }
  loadAlertState();   // pull the undelivered-alert flag from flash (survives any reset)

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

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
  if (newPress) {
    alertPending = true;
    alertSeq     = ++seqCounter;     // one seq for this alert; reused across retries and resets
    saveAlertState();                // PERSIST to flash NOW, before any send -- so even a
                                     // brownout/power-loss during the first transmit can't lose it
  }

  Message msg = {};
  msg.version    = PROTO_VERSION;
  msg.device_id  = DEVICE_ID;
  msg.battery_mv = readBatteryMv();

  if (alertPending) {
    // Deliver (or RESUME delivering) the pending alert. Because alertPending/alertSeq
    // live in FLASH, a reset/brownout/power-loss mid-alert lands us right back here on
    // reboot and we keep going — the alert is never abandoned. We stay awake and retry
    // continuously until the RX confirms the latch (battery trade-off accepted by design,
    // see HANDOFF §alert). seq is fixed, so the idempotent RX latch tolerates duplicates.
    msg.type = MSG_ALERT;
    msg.seq  = alertSeq;
    Serial.printf("ALERT seq=%lu batt=%u — sending, will retry until acked (persists across resets)\n",
                  (unsigned long)msg.seq, msg.battery_mv);
    uint32_t round = 0;
    while (!sendMessage(msg)) {       // sendMessage = one burst of retries; loop = forever
      round++;
      Serial.printf("[tx] ALERT not yet acked (round %lu) — retrying\n", (unsigned long)round);
      blink(6, 300, 150);            // urgent: NOT confirmed yet -> seek help another way
    }
    alertPending = false;            // RX confirmed the latch -> the alert is delivered
    saveAlertState();                // clear the persisted flag so we don't resume after reboot
    Serial.println("[tx] ALERT CONFIRMED (ack received)");
    blink(2, 60, 80);                // confirmed: two quick blinks

    // Wait for a clean, debounced RELEASE so bounce can't immediately re-arm EXT1.
    // If it never releases, goToSleep() sleeps on the timer only (won't arm a held pad).
    waitButtonStable(HIGH, RELEASE_WAIT_MS);
  } else {
    // Timer heartbeat OR cold boot / reset / brownout / debounced-away glitch -> heartbeat.
    msg.type = MSG_HEARTBEAT;
    msg.seq  = ++seqCounter;
    Serial.printf("HEARTBEAT seq=%lu batt=%u (wakeup_cause=%d) ... ",
                  (unsigned long)msg.seq, msg.battery_mv, (int)cause);
    bool ok = sendMessage(msg);
    Serial.println(ok ? "ack" : "no ack");
  }

  goToSleep();
}

void loop() {}   // unreached: all logic runs in setup() before deep sleep
