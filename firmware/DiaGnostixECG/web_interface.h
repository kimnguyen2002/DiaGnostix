#pragma once

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
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

// Wi-Fi access point "DiaGnostix" with a captive portal, an optional second connection to a home
// network (secrets.h), mDNS, the dashboard over HTTP and the WebSocket endpoint /ws.
//
// Captive portal: a DNS server answers every name with the device's address, and the connectivity
// checks that phones and laptops make after joining a network (Android /generate_204, Apple
// /hotspot-detect.html, Windows /connecttest.txt, Firefox /canonical.html, ...) are redirected to the
// dashboard, which opens on its sign-in page. The network has no internet access by design.
class WebInterface {
 public:
  // Incoming WebSocket text frames are posted to command_queue as RawCommand.
  bool begin(QueueHandle_t command_queue);
  void loop();

  void sendTo(uint32_t client_id, const String& msg) { ws_.text(client_id, msg); }
  void sendTo(uint32_t client_id, const char* msg, size_t len) { ws_.text(client_id, msg, len); }

  // Clients that connected / disconnected since the last call (the main loop tracks who is signed in).
  bool takeNewClient(uint32_t* client_id) { return xQueueReceive(new_clients_, client_id, 0) == pdTRUE; }
  bool takeGoneClient(uint32_t* client_id) { return xQueueReceive(gone_clients_, client_id, 0) == pdTRUE; }

  // JSON returned by GET /api/state (device status only, no personal data); refreshed by the main loop.
  void setStateSnapshot(const String& json);

  String apAddress() const { return WiFi.softAPIP().toString(); }
  bool stationConnected() const { return WiFi.status() == WL_CONNECTED; }

 private:
  void startWifi();
  void addCaptivePortalRoutes();
  void onEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);
  bool isOwnHost(AsyncWebServerRequest* request) const;
  void redirectToPortal(AsyncWebServerRequest* request) const;

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  DNSServer dns_;
  QueueHandle_t command_queue_ = nullptr;
  QueueHandle_t new_clients_ = nullptr;
  QueueHandle_t gone_clients_ = nullptr;
  SemaphoreHandle_t snapshot_lock_ = nullptr;
  String snapshot_ = "{}";
  uint32_t last_cleanup_ms_ = 0;
};

inline bool WebInterface::begin(QueueHandle_t command_queue) {
  command_queue_ = command_queue;
  new_clients_ = xQueueCreate(8, sizeof(uint32_t));
  gone_clients_ = xQueueCreate(8, sizeof(uint32_t));
  snapshot_lock_ = xSemaphoreCreateMutex();
  if (!new_clients_ || !gone_clients_ || !snapshot_lock_) return false;

  startWifi();
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(53, "*", WiFi.softAPIP());  // every name resolves to the device: captive portal

  ws_.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                     size_t len) { onEvent(client, type, arg, data, len); });
  server_.addHandler(&ws_);

  // The dashboard (web/index.html) is stored gzip-compressed in dashboard_html.h.
  server_.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/html; charset=utf-8", kDashboardHtmlGz, kDashboardHtmlGzLen);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest* request) {
    String body;
    if (xSemaphoreTake(snapshot_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
      body = snapshot_;
      xSemaphoreGive(snapshot_lock_);
    }
    request->send(200, "application/json", body);
  });
  addCaptivePortalRoutes();
  server_.onNotFound([this](AsyncWebServerRequest* request) {
    if (isOwnHost(request)) {
      request->send(404, "text/plain", "Not found");
    } else {
      redirectToPortal(request);  // any other site: show the sign-in page
    }
  });
  server_.begin();
  return true;
}

inline void WebInterface::addCaptivePortalRoutes() {
  // Connectivity checks of Android/Chrome OS, Apple, Windows and Firefox. Answering with a redirect
  // (instead of the expected 204 / "Success") makes the operating system open the portal.
  static const char* const kProbes[] = {"/generate_204", "/gen_204",   "/hotspot-detect.html",
                                        "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
                                        "/redirect",     "/canonical.html", "/success.txt", "/fwlink"};
  for (const char* p : kProbes) {
    server_.on(p, HTTP_ANY, [this](AsyncWebServerRequest* request) { redirectToPortal(request); });
  }
}

inline bool WebInterface::isOwnHost(AsyncWebServerRequest* request) const {
  const String host = request->host();
  return host == WiFi.softAPIP().toString() || host == String(WIFI_HOSTNAME) + ".local" ||
         (WiFi.status() == WL_CONNECTED && host == WiFi.localIP().toString());
}

inline void WebInterface::redirectToPortal(AsyncWebServerRequest* request) const {
  request->redirect("http://" + WiFi.softAPIP().toString() + "/");
}

inline void WebInterface::onEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                                  size_t len) {
  const uint32_t id = client->id();
  if (type == WS_EVT_CONNECT) {
    client->setCloseClientOnQueueFull(false);  // a slow phone drops waveform frames instead of the link
    xQueueSend(new_clients_, &id, 0);
  } else if (type == WS_EVT_DISCONNECT) {
    xQueueSend(gone_clients_, &id, 0);
  } else if (type == WS_EVT_DATA) {
    auto* info = static_cast<AwsFrameInfo*>(arg);
    // Commands are small: accept only complete, unfragmented text frames.
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      if (!RawCommand::post(command_queue_, RawCommand::Source::kWebSocket, id, data, len)) {
        client->text("{\"type\":\"error\",\"message\":\"Command rejected (too long or device busy)\"}");
      }
    }
  }
}

inline void WebInterface::startWifi() {
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  const bool home = std::strlen(WIFI_SSID) > 0;
  WiFi.mode(home ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  Serial.printf("[wifi] access point \"%s\" (password \"%s\"), portal http://%s/\n", AP_SSID, AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());
  if (home) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] also joining \"%s\"", WIFI_SSID);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);
      Serial.printf("[wifi] home network IP %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[wifi] home network not reachable; access point only");
    }
  }
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[wifi] dashboard: http://%s.local/\n", WIFI_HOSTNAME);
  }
}

inline void WebInterface::loop() {
  dns_.processNextRequest();
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
