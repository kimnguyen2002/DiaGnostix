#pragma once

#include <Arduino.h>
#include <FFat.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <cctype>
#include <cstring>
#include <vector>

#include "app_types.h"
#include "bp_estimator.h"
#include "config.h"

// User accounts and per-user data on the FAT partition of the N16R8
// (Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)"):
//   /u/<id>/acct   account: username, salt, PBKDF2-HMAC-SHA256 password hash
//   /u/<id>/prof   profile (age, sex, height, weight, lifestyle answers)
//   /u/<id>/bp     blood-pressure calibration (person-specific: pulse arrival time -> BP)
//   /u/<id>/hist   results, one JSON object per line, newest last (at most HISTORY_MAX_RESULTS)
// Passwords are never stored, only a salted hash. Accessed from loop() only.
class UserStore {
 public:
  bool begin();
  bool ok() const { return ok_; }
  int count() const;
  int find(const char* username) const;  // user id, or -1
  const char* username(int id) const { return valid(id) ? acct_[id].username : ""; }
  bool valid(int id) const { return id >= 0 && id < MAX_USERS && used_[id]; }

  // Creates an account; returns nullptr on success, otherwise the reason (shown to the user).
  const char* create(const char* username, const char* password, int* id);
  bool verify(int id, const char* password) const;
  void remove(int id);

  bool loadProfile(int id, UserProfile* p) const { return load(id, "prof", p) && p->valid; }
  void saveProfile(int id, const UserProfile& p) { save(id, "prof", p); }
  void clearProfile(int id) { erase(id, "prof"); }
  bool loadBpCalibration(int id, dsp::BpCalibration* c) const { return load(id, "bp", c); }
  void saveBpCalibration(int id, const dsp::BpCalibration& c) { save(id, "bp", c); }
  void clearBpCalibration(int id) { erase(id, "bp"); }

  void appendResult(int id, const String& json_line);
  String history(int id, int max_results) const;  // JSON array, oldest first

  static bool usernameValid(const char* u);

 private:
  struct Account {
    uint8_t version;
    char username[25];
    uint8_t salt[16];
    uint8_t hash[32];
    uint32_t iterations;
  };
  static constexpr uint8_t kVersion = 1;

  static String path(int id, const char* name) { return "/u/" + String(id) + "/" + name; }
  template <typename T>
  bool load(int id, const char* name, T* out) const;
  template <typename T>
  void save(int id, const char* name, const T& value);
  void erase(int id, const char* name);
  static bool pbkdf2(const char* password, const uint8_t* salt, size_t salt_len, uint32_t iterations, uint8_t out[32]);

  Account acct_[MAX_USERS] = {};
  bool used_[MAX_USERS] = {};
  bool ok_ = false;
};

// Signed-in browsers and phones: random 128-bit tokens, kept in RAM only (a reboot signs everyone out).
class LoginSessions {
 public:
  const char* create(int user, uint32_t now_s);  // token (32 hex characters)
  int resolve(const char* token, uint32_t now_s);  // user id or -1; extends the session
  void revoke(const char* token);
  void revokeUser(int user);

 private:
  struct Entry {
    char token[33];
    int user;
    uint32_t expires_s;
    bool used;
  };
  Entry e_[MAX_LOGIN_SESSIONS] = {};
};

// ---------------------------------------------------------------------------- implementation

inline bool UserStore::begin() {
  ok_ = FFat.begin(true);  // formats the partition on first use
  if (!ok_) return false;
  if (!FFat.exists("/u")) FFat.mkdir("/u");
  for (int id = 0; id < MAX_USERS; ++id) used_[id] = load(id, "acct", &acct_[id]) && acct_[id].version == kVersion;
  return true;
}

inline int UserStore::count() const {
  int n = 0;
  for (int id = 0; id < MAX_USERS; ++id) n += used_[id];
  return n;
}

inline bool UserStore::usernameValid(const char* u) {
  const size_t n = std::strlen(u);
  if (n < 3 || n > 24) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = u[i];
    if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
          c == '_' || c == '-'))
      return false;
  }
  return true;
}

inline int UserStore::find(const char* username) const {
  for (int id = 0; id < MAX_USERS; ++id)
    if (used_[id] && !std::strcmp(acct_[id].username, username)) return id;
  return -1;
}

inline const char* UserStore::create(const char* username, const char* password, int* id) {
  if (!ok_) return "User storage is not available";
  if (!usernameValid(username)) return "Username: 3-24 characters a-z, 0-9, '.', '_' or '-'";
  if (std::strlen(password) < PASSWORD_MIN_LENGTH) return "Password too short";
  if (find(username) >= 0) return "This username is already taken";
  int slot = -1;
  for (int i = 0; i < MAX_USERS && slot < 0; ++i)
    if (!used_[i]) slot = i;
  if (slot < 0) return "The device has reached its maximum number of users";
  Account a = {};
  a.version = kVersion;
  std::snprintf(a.username, sizeof(a.username), "%s", username);
  esp_fill_random(a.salt, sizeof(a.salt));
  a.iterations = PBKDF2_ITERATIONS;
  if (!pbkdf2(password, a.salt, sizeof(a.salt), a.iterations, a.hash)) return "Could not hash the password";
  FFat.mkdir("/u/" + String(slot));
  erase(slot, "prof");
  erase(slot, "bp");
  erase(slot, "hist");
  save(slot, "acct", a);
  acct_[slot] = a;
  used_[slot] = true;
  *id = slot;
  return nullptr;
}

inline bool UserStore::verify(int id, const char* password) const {
  if (!valid(id)) return false;
  uint8_t h[32];
  if (!pbkdf2(password, acct_[id].salt, sizeof(acct_[id].salt), acct_[id].iterations, h)) return false;
  uint8_t diff = 0;  // constant-time comparison
  for (int i = 0; i < 32; ++i) diff |= h[i] ^ acct_[id].hash[i];
  return diff == 0;
}

inline void UserStore::remove(int id) {
  if (!valid(id)) return;
  for (const char* f : {"acct", "prof", "bp", "hist"}) erase(id, f);
  FFat.rmdir("/u/" + String(id));
  used_[id] = false;
  acct_[id] = Account();
}

template <typename T>
inline bool UserStore::load(int id, const char* name, T* out) const {
  if (!ok_) return false;
  File f = FFat.open(path(id, name), FILE_READ);
  if (!f) return false;
  const bool good = f.size() == sizeof(T) && f.read(reinterpret_cast<uint8_t*>(out), sizeof(T)) == sizeof(T);
  f.close();
  return good;
}

template <typename T>
inline void UserStore::save(int id, const char* name, const T& value) {
  if (!ok_) return;
  File f = FFat.open(path(id, name), FILE_WRITE);
  if (!f) return;
  f.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
  f.close();
}

inline void UserStore::erase(int id, const char* name) {
  if (ok_ && FFat.exists(path(id, name))) FFat.remove(path(id, name));
}

inline void UserStore::appendResult(int id, const String& json_line) {
  if (!valid(id)) return;
  const String p = path(id, "hist");
  File f = FFat.open(p, FILE_APPEND);
  if (!f) return;
  f.print(json_line);
  f.print('\n');
  const size_t size = f.size();
  f.close();
  if (size < 400UL * HISTORY_MAX_RESULTS) return;
  // Compact: keep the newest HISTORY_MAX_RESULTS lines.
  std::vector<String> lines;
  File r = FFat.open(p, FILE_READ);
  while (r && r.available()) {
    String line = r.readStringUntil('\n');
    if (line.length()) lines.push_back(line);
  }
  r.close();
  const size_t start = lines.size() > HISTORY_MAX_RESULTS ? lines.size() - HISTORY_MAX_RESULTS : 0;
  File w = FFat.open(p, FILE_WRITE);
  for (size_t i = start; w && i < lines.size(); ++i) {
    w.print(lines[i]);
    w.print('\n');
  }
  w.close();
}

inline String UserStore::history(int id, int max_results) const {
  String out = "[";
  if (!valid(id)) return out + "]";
  std::vector<String> lines;
  File r = FFat.open(path(id, "hist"), FILE_READ);
  while (r && r.available()) {
    String line = r.readStringUntil('\n');
    if (!line.length()) continue;
    lines.push_back(line);
    if (static_cast<int>(lines.size()) > max_results) lines.erase(lines.begin());
  }
  r.close();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += ',';
    out += lines[i];
  }
  return out + "]";
}

// PBKDF2-HMAC-SHA256 (RFC 8018) for a single 32-byte block.
inline bool UserStore::pbkdf2(const char* password, const uint8_t* salt, size_t salt_len, uint32_t iterations,
                              uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || iterations == 0) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
            mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(password), std::strlen(password)) == 0;
  uint8_t u[32];
  const uint8_t block_index[4] = {0, 0, 0, 1};
  ok = ok && mbedtls_md_hmac_update(&ctx, salt, salt_len) == 0 && mbedtls_md_hmac_update(&ctx, block_index, 4) == 0 &&
       mbedtls_md_hmac_finish(&ctx, u) == 0;
  std::memcpy(out, u, 32);
  for (uint32_t i = 1; ok && i < iterations; ++i) {
    ok = mbedtls_md_hmac_reset(&ctx) == 0 && mbedtls_md_hmac_update(&ctx, u, 32) == 0 &&
         mbedtls_md_hmac_finish(&ctx, u) == 0;
    for (int j = 0; j < 32; ++j) out[j] ^= u[j];
  }
  mbedtls_md_free(&ctx);
  return ok;
}

inline const char* LoginSessions::create(int user, uint32_t now_s) {
  int slot = 0;
  for (int i = 0; i < MAX_LOGIN_SESSIONS; ++i) {  // free slot, else the one that expires first
    if (!e_[i].used || e_[i].expires_s < now_s) {
      slot = i;
      break;
    }
    if (e_[i].expires_s < e_[slot].expires_s) slot = i;
  }
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  for (int i = 0; i < 16; ++i) std::snprintf(&e_[slot].token[2 * i], 3, "%02x", r[i]);
  e_[slot].user = user;
  e_[slot].expires_s = now_s + LOGIN_SESSION_TTL_S;
  e_[slot].used = true;
  return e_[slot].token;
}

inline int LoginSessions::resolve(const char* token, uint32_t now_s) {
  if (!token || std::strlen(token) != 32) return -1;
  for (Entry& e : e_) {
    if (e.used && e.expires_s >= now_s && !std::strcmp(e.token, token)) {
      e.expires_s = now_s + LOGIN_SESSION_TTL_S;
      return e.user;
    }
  }
  return -1;
}

inline void LoginSessions::revoke(const char* token) {
  for (Entry& e : e_)
    if (e.used && !std::strcmp(e.token, token)) e.used = false;
}

inline void LoginSessions::revokeUser(int user) {
  for (Entry& e : e_)
    if (e.used && e.user == user) e.used = false;
}
