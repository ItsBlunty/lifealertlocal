// Local Life-Alert — TRANSMITTER (worn/mounted button)
//
// Deep-sleeps; wakes on a button press (-> ALERT) or the heartbeat timer
// (-> HEARTBEAT); sends, waits for an APP-LEVEL ack, gives feedback, sleeps.
// A reset / power-on / brownout must NEVER fire the alarm (requirement #6):
// only an EXT0 (button) wake sends an ALERT; everything else is a heartbeat.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include "common.h"

// ---------------- CONFIG ----------------
uint8_t RX_MAC[6] = {0x44, 0x1D, 0x64, 0xF5, 0x87, 0xF8};   // receiver (board #1)

#define BUTTON_GPIO         GPIO_NUM_0   // BOOT button (strapping pin; prototype only, see README)
#define LED_GPIO            2            // onboard LED = confirm / failure feedback
#define HEARTBEAT_SECONDS   20           // TESTING value. TODO: 300 (5 min) for deployment. MUST match RX.
#define ACK_TIMEOUT_MS      400          // wait for app-level ACK per attempt
#define MAX_SEND_ATTEMPTS   20           // persistence for an alert
#define DEVICE_ID           1

// Battery sense is OFF until a divider is wired (guide §3).
//   BATTERY_SENSE_ENABLED 0 -> report FAKE_BATTERY_MV.
//        FAKE_BATTERY_MV 0    -> "N/A": RX never raises a (false) low-battery warning.
//        FAKE_BATTERY_MV 3200 -> exercise the RX low-battery path end-to-end (it's < LOW_BATT_MV).
//   BATTERY_SENSE_ENABLED 1 -> read the real divider on BATTERY_ADC_PIN (must be ADC1: GPIO32-39).
#define BATTERY_SENSE_ENABLED  0
#define FAKE_BATTERY_MV        0
#define BATTERY_ADC_PIN        34
#define BATTERY_DIVIDER        2.0f      // (R1+R2)/R2 — match your divider, then calibrate vs a multimeter
// ----------------------------------------

RTC_DATA_ATTR uint32_t seqCounter = 0;   // survives deep sleep

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

void blink(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_GPIO, HIGH); delay(onMs);
    digitalWrite(LED_GPIO, LOW);  delay(offMs);
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

void goToSleep() {
  Serial.flush();
  esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);   // 0 = wake when GPIO0 goes LOW
  esp_sleep_enable_timer_wakeup((uint64_t)HEARTBEAT_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(LED_GPIO, OUTPUT); digitalWrite(LED_GPIO, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  WiFi.mode(WIFI_STA);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
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

  Message msg = {};
  msg.version    = PROTO_VERSION;
  msg.device_id  = DEVICE_ID;
  msg.seq        = ++seqCounter;
  msg.battery_mv = readBatteryMv();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // REAL BUTTON PRESS -> alert
    msg.type = MSG_ALERT;
    Serial.printf("ALERT seq=%lu batt=%u ... ", (unsigned long)msg.seq, msg.battery_mv);
    bool ok = sendMessage(msg);
    Serial.println(ok ? "CONFIRMED (ack received)" : "FAILED (no ack after retries)");
    if (ok) blink(2, 60, 80);        // confirmed: two quick blinks
    else    blink(6, 300, 150);      // FAILED: long urgent pattern -> seek help another way

    // Don't immediately re-trigger if the button is still held down.
    pinMode(0, INPUT);
    uint32_t tr = millis();
    while (digitalRead(0) == LOW && millis() - tr < 5000) delay(10);
  } else {
    // Timer heartbeat OR cold boot / reset / brownout -> heartbeat only.
    msg.type = MSG_HEARTBEAT;
    Serial.printf("HEARTBEAT seq=%lu batt=%u (wakeup_cause=%d) ... ",
                  (unsigned long)msg.seq, msg.battery_mv, (int)cause);
    bool ok = sendMessage(msg);
    Serial.println(ok ? "ack" : "no ack");
  }

  goToSleep();
}

void loop() {}   // unreached: all logic runs in setup() before deep sleep
