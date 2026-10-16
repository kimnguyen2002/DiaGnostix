#pragma once

#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <cstring>

#include "command_queue.h"
#include "config.h"
#include "dashboard_html.h"

// Wi-Fi (station, falling back to an access point), mDNS, the dashboard over
// HTTP and the WebSocket endpoint /ws.
class WebInterface {
 public:
  // Incoming WebSocket text frames are posted to command_queue as RawCommand.
  bool begin(QueueHandle_t command_queue);
  void loop();

  void broadcast(const String& msg) {
    if (ws_.count() > 0) ws_.textAll(msg);
  }
  void broadcast(const char* msg, size_t len) {
    if (ws_.count() > 0) ws_.textAll(msg, len);
  }
  void sendTo(uint32_t client_id, const String& msg) { ws_.text(client_id, msg); }
  bool hasClients() { return ws_.count() > 0; }

  // Id of a client that connected since the last call (to greet it).
  bool takeNewClient(uint32_t* client_id) { return xQueueReceive(new_clients_, client_id, 0) == pdTRUE; }

  // JSON returned by GET /api/state; refreshed by the main loop.
  void setStateSnapshot(const String& json);

  String address() const { return ap_mode_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }
  bool accessPointMode() const { return ap_mode_; }

 private:
  void startWifi();
  void onEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  QueueHandle_t command_queue_ = nullptr;
  QueueHandle_t new_clients_ = nullptr;
  SemaphoreHandle_t snapshot_lock_ = nullptr;
  String snapshot_ = "{}";
  bool ap_mode_ = false;
  uint32_t last_cleanup_ms_ = 0;
};

inline bool WebInterface::begin(QueueHandle_t command_queue) {
  command_queue_ = command_queue;
  new_clients_ = xQueueCreate(8, sizeof(uint32_t));
  snapshot_lock_ = xSemaphoreCreateMutex();
  if (!new_clients_ || !snapshot_lock_) return false;

  startWifi();

  ws_.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                     size_t len) { onEvent(client, type, arg, data, len); });
  server_.addHandler(&ws_);

  // The dashboard (web/index.html) is stored gzip-compressed in dashboard_html.h.
  server_.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/html; charset=utf-8", kDashboardHtmlGz, kDashboardHtmlGzLen);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });
  server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest* request) {
    String body;
    if (xSemaphoreTake(snapshot_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
      body = snapshot_;
      xSemaphoreGive(snapshot_lock_);
    }
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", body);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });
  server_.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "Not found"); });
  server_.begin();
  return true;
}

inline void WebInterface::onEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                                  size_t len) {
  if (type == WS_EVT_CONNECT) {
    const uint32_t id = client->id();
    xQueueSend(new_clients_, &id, 0);
  } else if (type == WS_EVT_DATA) {
    auto* info = static_cast<AwsFrameInfo*>(arg);
    // Commands are small: accept only complete, unfragmented text frames.
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      if (!RawCommand::post(command_queue_, RawCommand::Source::kWebSocket, client->id(), data, len)) {
        client->text("{\"type\":\"error\",\"message\":\"Command rejected (too long or device busy)\"}");
      }
    }
  }
}

inline void WebInterface::startWifi() {
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  if (std::strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    ap_mode_ = false;
    Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    ap_mode_ = true;
    Serial.printf("[wifi] access point \"%s\" (password \"%s\"), IP %s\n", AP_SSID, AP_PASSWORD,
                  WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[wifi] dashboard: http://%s.local/  or  http://%s/\n", WIFI_HOSTNAME, address().c_str());
  }
}

inline void WebInterface::loop() {
  if (millis() - last_cleanup_ms_ > 1000) {
    last_cleanup_ms_ = millis();
    ws_.cleanupClients();
  }
}

inline void WebInterface::setStateSnapshot(const String& json) {
  if (xSemaphoreTake(snapshot_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapshot_ = json;
    xSemaphoreGive(snapshot_lock_);
  }
}
