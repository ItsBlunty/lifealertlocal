// Local Life-Alert — RECEIVER (mains-powered alarm)
//
// Always awake. Latches the alarm on ALERT (a momentary press creates a
// persistent alarm until a human clears it at the receiver). Tracks liveness
// and battery from heartbeats and drives three distinguishable warnings plus idle.
//
// NO PERIPHERALS WIRED YET: buzzer (GPIO25) and alarm LED (GPIO26) are kept for
// the real build, but right now verification is via the SERIAL LOG (every state
// transition is printed) and the onboard STATUS LED (GPIO2), which mirrors the
// current state so you get visible feedback on a bare dev board.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "common.h"

// ---------------- CONFIG ----------------
uint8_t TX_MAC[6] = {0xE0, 0x72, 0xA1, 0xF9, 0x54, 0x1C};   // transmitter (board #2 = XIAO ESP32-S3)

#define BUZZER_GPIO        25         // active buzzer (digitalWrite). Wire later.
#define ALARM_LED_GPIO     26         // alarm LED. Wire later.
#define STATUS_LED_GPIO    2          // onboard LED — also the no-buzzer test indicator
#define CLEAR_BUTTON_GPIO  0          // BOOT button clears the alarm (prototype)
#define HEARTBEAT_SECONDS  300        // deployment value (5 min). MUST match TX. Offline = this * OFFLINE_MULT.
#define OFFLINE_MULT       3          // offline after this many missed heartbeats
#define LOW_BATT_MV        3300       // tune to battery chemistry (with margin above cutoff)
// ----------------------------------------

volatile bool     alarmLatched   = false;
volatile uint32_t lastHeartbeatMs = 0;
volatile uint16_t lastBatteryMv  = 0;
volatile bool     batteryLow     = false;
// The TX resends a FIXED seq for the whole life of one alert and stays awake until we tell
// it (via the ACK flag) that the alarm is no longer active. currentAlarmSeq = the seq that
// is currently latched; clearedSeq = an alert seq an operator has cleared, which we must NOT
// re-latch when the still-awake TX resends it (a genuinely new press carries a new seq).
volatile uint32_t currentAlarmSeq = 0;
volatile uint32_t clearedSeq      = 0;

void sendAck(const uint8_t *dst, uint32_t seq, uint8_t flags = 0) {
  Message a = {}; a.version = PROTO_VERSION; a.type = MSG_ACK; a.seq = seq; a.flags = flags;
  esp_now_send(dst, (const uint8_t *)&a, sizeof(a));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(Message)) return;
  Message m; memcpy(&m, data, sizeof(m));
  if (m.version != PROTO_VERSION) return;

  lastHeartbeatMs = millis();
  if (m.battery_mv) { lastBatteryMv = m.battery_mv; batteryLow = (m.battery_mv < LOW_BATT_MV); }

  if (m.type == MSG_ALERT) {
    // The TX stays awake and resends this same seq until our ACK says the alarm is no
    // longer active. If an operator has already cleared THIS seq, don't re-latch it — just
    // ACK it as cleared so the TX can stop and drop back to its low-power heartbeat cycle.
    bool active = (m.seq != clearedSeq);
    if (active) {
      if (!alarmLatched || m.seq != currentAlarmSeq)   // log once per new alert, not every resend
        Serial.printf("[rx] ALERT seq=%lu batt=%u -> alarm LATCHED\n",
                      (unsigned long)m.seq, m.battery_mv);
      alarmLatched    = true;
      currentAlarmSeq = m.seq;
    }
    sendAck(info->src_addr, m.seq, active ? ACK_ALARM_ACTIVE : 0);
  } else if (m.type == MSG_HEARTBEAT) {
    // Dedup the LOG only: a healthy TX sends each seq once (logged once). A wedged TX
    // that repeats one seq gets collapsed to a single line plus a suppressed-count
    // summary, so a flood can't drown the serial log. Liveness + ACK still act on EVERY
    // frame (above), so nothing functional is dropped. A TX reset (seq -> 1) is a new
    // seq, so it logs normally rather than being mistaken for a duplicate.
    static uint32_t lastHbSeq = 0;
    static bool     haveHb    = false;
    static uint32_t dupCount  = 0;
    if (haveHb && m.seq == lastHbSeq) {
      dupCount++;
    } else {
      if (dupCount > 0)
        Serial.printf("[rx]   (suppressed %lu duplicate heartbeat frames of seq=%lu)\n",
                      (unsigned long)dupCount, (unsigned long)lastHbSeq);
      dupCount = 0; lastHbSeq = m.seq; haveHb = true;
      Serial.printf("[rx] heartbeat seq=%lu batt=%u%s\n",
                    (unsigned long)m.seq, m.battery_mv, batteryLow ? " (LOW)" : "");
    }
    sendAck(info->src_addr, m.seq);
  }
}

// ---- state machine: ALARM > OFFLINE > LOW-BATTERY > IDLE ----
enum RxState { ST_IDLE, ST_ALARM, ST_OFFLINE, ST_LOWBATT };
const char *stateName(RxState s) {
  switch (s) {
    case ST_ALARM:   return "ALARM";
    case ST_OFFLINE: return "OFFLINE";
    case ST_LOWBATT: return "LOW-BATTERY";
    default:         return "IDLE";
  }
}

void buzz(bool on) { digitalWrite(BUZZER_GPIO, on ? HIGH : LOW); }

void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(BUZZER_GPIO, OUTPUT);
  pinMode(ALARM_LED_GPIO, OUTPUT);
  pinMode(STATUS_LED_GPIO, OUTPUT);
  pinMode(CLEAR_BUTTON_GPIO, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  { uint8_t pc; wifi_second_chan_t sc; esp_wifi_get_channel(&pc, &sc);
    Serial.printf("[rx] wifi channel = %u (want %u)\n", pc, ESPNOW_CHANNEL); }
  if (esp_now_init() != ESP_OK) Serial.println("esp_now_init failed");
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};       // add TX so we can ACK back to it
  memcpy(peer.peer_addr, TX_MAC, 6);
  peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

  lastHeartbeatMs = millis();          // grace period at startup (don't fire OFFLINE immediately)
  Serial.print("RX MAC (put this in RX_MAC[] of tx/src/main.cpp): ");
  Serial.println(WiFi.macAddress());
  Serial.println("[rx] ready, state=IDLE");
}

void loop() {
  uint32_t now = millis();

  // clear button (active low, debounced)
  static uint32_t lastPress = 0;
  if (digitalRead(CLEAR_BUTTON_GPIO) == LOW && now - lastPress > 300) {
    lastPress = now;
    if (alarmLatched) {
      alarmLatched = false;
      clearedSeq   = currentAlarmSeq;   // remember it so the still-awake TX's resends of this
                                        // same seq are ACKed as cleared instead of re-latching
      Serial.println("[rx] alarm CLEARED by button (TX will get the cleared ACK and stand down)");
    }
  }

  bool offline = (now - lastHeartbeatMs) >
                 (uint32_t)HEARTBEAT_SECONDS * OFFLINE_MULT * 1000UL;

  RxState state = alarmLatched ? ST_ALARM
                : offline      ? ST_OFFLINE
                : batteryLow   ? ST_LOWBATT
                :                ST_IDLE;

  // log on every transition (and once at startup)
  static RxState prev = ST_IDLE;
  static bool firstReport = true;
  if (state != prev || firstReport) {
    Serial.printf("[rx] state -> %s\n", stateName(state));
    prev = state; firstReport = false;
  }

  switch (state) {
    case ST_ALARM:                                   // urgent: ~2 Hz continuous, LED solid
      digitalWrite(ALARM_LED_GPIO, HIGH);
      digitalWrite(STATUS_LED_GPIO, HIGH);
      buzz((now / 250) % 2);
      break;
    case ST_OFFLINE: {                               // double-chirp every ~8 s
      digitalWrite(ALARM_LED_GPIO, LOW);
      uint32_t p = now % 8000;
      bool on = (p < 80) || (p > 200 && p < 280);
      buzz(on);
      digitalWrite(STATUS_LED_GPIO, on);
      break;
    }
    case ST_LOWBATT: {                               // single short chirp every ~30 s
      digitalWrite(ALARM_LED_GPIO, LOW);
      bool on = (now % 30000) < 80;
      buzz(on);
      digitalWrite(STATUS_LED_GPIO, on);
      break;
    }
    default:                                         // IDLE: silent, brief status blink / 3 s
      digitalWrite(ALARM_LED_GPIO, LOW);
      buzz(false);
      digitalWrite(STATUS_LED_GPIO, (now % 3000) < 40);
      break;
  }
}
