#pragma once

#include <Preferences.h>

#include "app_types.h"

// Persists the device-level settings in NVS flash: the AD8232 gain calibration (a property of the
// board) and the result counter. Per-user data (profile, blood-pressure calibration, results) is in
// user_store.h.
class SettingsStore {
 public:
  bool begin();

  bool loadGain(EcgGain* g);
  void saveGain(const EcgGain& g);
  void clearGain();

  uint32_t nextResultId();

 private:
  // Bump when the layout of a stored struct changes; old records are ignored.
  // 2: DiaGnostix-ECG profile (lifestyle answers instead of the DiaBD questionnaire).
  static constexpr uint8_t kSchemaVersion = 2;

  template <typename T>
  bool load(const char* key, T* out);
  template <typename T>
  void save(const char* key, const T& value);

  Preferences prefs_;
  bool ok_ = false;
};

namespace settings_detail {
template <typename T>
struct Record {
  uint8_t version;
  T value;
};
}  // namespace settings_detail

inline bool SettingsStore::begin() {
  ok_ = prefs_.begin("diagnostix", false);
  return ok_;
}

template <typename T>
inline bool SettingsStore::load(const char* key, T* out) {
  if (!ok_) return false;
  settings_detail::Record<T> rec;
  if (prefs_.getBytesLength(key) != sizeof(rec)) return false;
  prefs_.getBytes(key, &rec, sizeof(rec));
  if (rec.version != kSchemaVersion) return false;
  *out = rec.value;
  return true;
}

template <typename T>
inline void SettingsStore::save(const char* key, const T& value) {
  if (!ok_) return;
  settings_detail::Record<T> rec{kSchemaVersion, value};
  prefs_.putBytes(key, &rec, sizeof(rec));
}

inline bool SettingsStore::loadGain(EcgGain* g) {
  return load("ecg_gain", g) && g->gain >= ECG_GAIN_MIN && g->gain <= ECG_GAIN_MAX;
}
inline void SettingsStore::saveGain(const EcgGain& g) { save("ecg_gain", g); }
inline void SettingsStore::clearGain() {
  if (ok_) prefs_.remove("ecg_gain");
}

inline uint32_t SettingsStore::nextResultId() {
  if (!ok_) return 0;
  const uint32_t id = prefs_.getUInt("result_id", 0) + 1;
  prefs_.putUInt("result_id", id);
  return id;
}
