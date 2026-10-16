#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config.h"

#if ENABLE_BLE

#include <NimBLEDevice.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "command_queue.h"

// Bluetooth LE GATT server for the mobile app (docs/protocol.md). JSON
// messages are split into frames that fit the negotiated MTU; each frame
// starts with a header byte (bit 7 = last frame, bits 0-6 = frame index).
class BleInterface {
 public:
  bool begin(QueueHandle_t command_queue, const String& info_json);

  void notifyTelemetry(const String& json) { notifyFramed(telemetry_, json); }
  void notifyEvent(const String& json) { notifyFramed(events_, json); }
  void setInfo(const String& json) {
    if (info_) info_->setValue(json.c_str());
  }
  bool connected() const { return server_ && server_->getConnectedCount() > 0; }

 private:
  static constexpr const char* kServiceUuid = "6e1d0001-8b3c-4a8e-9c4b-5f2d1a7c3e90";
  static constexpr const char* kTelemetryUuid = "6e1d0002-8b3c-4a8e-9c4b-5f2d1a7c3e90";
  static constexpr const char* kEventsUuid = "6e1d0003-8b3c-4a8e-9c4b-5f2d1a7c3e90";
  static constexpr const char* kCommandUuid = "6e1d0004-8b3c-4a8e-9c4b-5f2d1a7c3e90";
  static constexpr const char* kInfoUuid = "6e1d0005-8b3c-4a8e-9c4b-5f2d1a7c3e90";

  class ServerCallbacks : public NimBLEServerCallbacks {
   public:
    explicit ServerCallbacks(BleInterface* owner) : owner_(owner) {}
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
      Serial.printf("[ble] connected %s\n", info.getAddress().toString().c_str());
    }
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int reason) override {
      Serial.printf("[ble] disconnected (reason %d)\n", reason);
      if (server->getConnectedCount() == 0) owner_->mtu_.store(23);
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
      // Frames must fit the smallest MTU among connected peers.
      owner_->mtu_.store(owner_->server_->getConnectedCount() <= 1 ? mtu
                                                                    : std::min<uint16_t>(owner_->mtu_.load(), mtu));
    }

   private:
    BleInterface* owner_;
  };

  class CommandCallbacks : public NimBLECharacteristicCallbacks {
   public:
    explicit CommandCallbacks(BleInterface* owner) : owner_(owner) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      const NimBLEAttValue v = c->getValue();
      RawCommand::post(owner_->command_queue_, RawCommand::Source::kBle, 0, v.data(), v.length());
    }

   private:
    BleInterface* owner_;
  };

  void notifyFramed(NimBLECharacteristic* c, const String& json);

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* telemetry_ = nullptr;
  NimBLECharacteristic* events_ = nullptr;
  NimBLECharacteristic* info_ = nullptr;
  QueueHandle_t command_queue_ = nullptr;
  std::atomic<uint16_t> mtu_{23};
  ServerCallbacks server_callbacks_{this};
  CommandCallbacks command_callbacks_{this};
};

inline bool BleInterface::begin(QueueHandle_t command_queue, const String& info_json) {
  command_queue_ = command_queue;
  NimBLEDevice::init(DIAGNOSTIX_DEVICE_NAME);
  NimBLEDevice::setMTU(517);

  server_ = NimBLEDevice::createServer();
  server_->setCallbacks(&server_callbacks_, false);
  server_->advertiseOnDisconnect(true);

  NimBLEService* service = server_->createService(kServiceUuid);
  telemetry_ = service->createCharacteristic(kTelemetryUuid, NIMBLE_PROPERTY::NOTIFY);
  events_ = service->createCharacteristic(kEventsUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  NimBLECharacteristic* command =
      service->createCharacteristic(kCommandUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  command->setCallbacks(&command_callbacks_);
  info_ = service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
  info_->setValue(info_json.c_str());

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DIAGNOSTIX_DEVICE_NAME);
  adv->addServiceUUID(kServiceUuid);
  adv->enableScanResponse(true);
  const bool ok = adv->start();
  Serial.printf("[ble] advertising as \"%s\": %s\n", DIAGNOSTIX_DEVICE_NAME, ok ? "ok" : "FAILED");
  return ok;
}

inline void BleInterface::notifyFramed(NimBLECharacteristic* c, const String& json) {
  if (!c || !connected()) return;
  const size_t payload = std::max<int>(1, static_cast<int>(mtu_.load()) - 3 - 1);
  const size_t total = json.length();
  const uint8_t* data = reinterpret_cast<const uint8_t*>(json.c_str());
  uint8_t frame[512];
  size_t offset = 0;
  uint8_t index = 0;
  do {
    const size_t n = std::min(payload, std::min(total - offset, sizeof(frame) - 1));
    const bool last = offset + n >= total;
    frame[0] = static_cast<uint8_t>((last ? 0x80 : 0x00) | (index & 0x7F));
    std::memcpy(frame + 1, data + offset, n);
    c->setValue(frame, n + 1);
    c->notify();
    offset += n;
    ++index;
  } while (offset < total);
}

#else  // ENABLE_BLE

// Bluetooth disabled in config.h: every call is a no-op and the NimBLE library
// is not needed.
class BleInterface {
 public:
  bool begin(QueueHandle_t, const String&) { return false; }
  void notifyTelemetry(const String&) {}
  void notifyEvent(const String&) {}
  void setInfo(const String&) {}
  bool connected() const { return false; }
};

#endif  // ENABLE_BLE
