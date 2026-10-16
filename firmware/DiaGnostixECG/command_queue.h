#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstring>

// Raw JSON commands from the network tasks (WebSocket, BLE) to the main loop,
// which is the only place where application state is modified.
// The sender is recorded so that replies (hello, errors) go back to it only.
struct RawCommand {
  enum class Source : uint8_t { kWebSocket, kBle };
  static constexpr size_t kMaxLen = 512;
  Source source;
  uint32_t client_id;  // WebSocket client id; unused for BLE
  uint16_t len;
  char json[kMaxLen];

  static bool post(QueueHandle_t q, Source source, uint32_t client_id, const uint8_t* data, size_t len) {
    if (!q || len == 0 || len >= kMaxLen) return false;
    RawCommand c;
    c.source = source;
    c.client_id = client_id;
    c.len = static_cast<uint16_t>(len);
    std::memcpy(c.json, data, len);
    c.json[len] = '\0';
    return xQueueSend(q, &c, 0) == pdTRUE;
  }
};
