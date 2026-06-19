#pragma once
#include <stdint.h>

// Keep this file IDENTICAL in tx/ and rx/.

#define PROTO_VERSION   1
#define ESPNOW_CHANNEL  1   // 1-13. BOTH devices MUST use the same channel.

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
  uint16_t battery_mv;  // battery millivolts (0 = not available / N/A)
  uint16_t _pad2;
} Message;
