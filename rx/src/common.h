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

// Flags carried in Message.flags. Currently only on a MSG_ACK reply (RX -> TX): the RX
// reports whether the alarm is still latched/active, so the TX knows when an operator has
// CLEARED it. 0 on all other message types.
#define ACK_ALARM_ACTIVE  0x01   // RX alarm is latched/active; UNSET once the operator clears it

typedef struct __attribute__((packed)) {
  uint8_t  version;     // = PROTO_VERSION
  uint8_t  type;        // MsgType
  uint8_t  device_id;   // which button (1 for now; room to add more later)
  uint8_t  flags;       // MSG_ACK: ACK_* alarm-state bits (RX->TX). 0 on other types.
  uint32_t seq;         // monotonic sequence number
  uint16_t battery_mv;  // battery millivolts (0 = not available / N/A)
  uint16_t _pad2;
} Message;
