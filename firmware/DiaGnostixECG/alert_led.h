#pragma once

#include <cstdint>
#include <Arduino.h>

// Non-blocking LED (and optional buzzer) patterns, advanced from loop().
//
//   kRiskAlert       4 Hz flashing until acknowledged (high risk tier: see a doctor for a blood test)
//   kSafetyAlert     double blink every second (low SpO2 or fever)
//   kResultElevated  1 Hz blinking for 8 s, then off (elevated risk tier)
//   kResultNegative  solid for 2 s, then off (low risk tier)
//   kError           three slow blinks, then off
//   kIdle            off; blip() flashes briefly on every heartbeat
class AlertLed {
 public:
  enum class Pattern : uint8_t { kIdle, kRiskAlert, kSafetyAlert, kResultElevated, kResultNegative, kError };

  void begin(int led_pin, int buzzer_pin);
  void setPattern(Pattern p, uint32_t now_ms);
  Pattern pattern() const { return pattern_; }
  void blip(uint32_t now_ms);
  void update(uint32_t now_ms);

 private:
  void write(bool led_on, bool buzzer_on);

  int led_pin_ = -1;
  int buzzer_pin_ = -1;
  Pattern pattern_ = Pattern::kIdle;
  uint32_t since_ms_ = 0;
  uint32_t blip_until_ms_ = 0;
};

inline void AlertLed::begin(int led_pin, int buzzer_pin) {
  led_pin_ = led_pin;
  buzzer_pin_ = buzzer_pin;
  if (led_pin_ >= 0) pinMode(led_pin_, OUTPUT);
  if (buzzer_pin_ >= 0) pinMode(buzzer_pin_, OUTPUT);
  write(false, false);
}

inline void AlertLed::setPattern(Pattern p, uint32_t now_ms) {
  if (p == pattern_) return;
  pattern_ = p;
  since_ms_ = now_ms;
}

inline void AlertLed::blip(uint32_t now_ms) { blip_until_ms_ = now_ms + 40; }

inline void AlertLed::update(uint32_t now_ms) {
  const uint32_t t = now_ms - since_ms_;
  switch (pattern_) {
    case Pattern::kRiskAlert: {
      const bool led = (t % 250) < 125;
      const bool beep = (t % 2000) < 150;
      write(led, beep);
      break;
    }
    case Pattern::kSafetyAlert: {
      const uint32_t p = t % 1000;
      const bool on = p < 100 || (p >= 200 && p < 300);
      write(on, on);
      break;
    }
    case Pattern::kResultElevated:
      if (t < 8000) {
        write((t % 1000) < 500, false);
      } else {
        pattern_ = Pattern::kIdle;
        write(false, false);
      }
      break;
    case Pattern::kResultNegative:
      if (t < 2000) {
        write(true, false);
      } else {
        pattern_ = Pattern::kIdle;
        write(false, false);
      }
      break;
    case Pattern::kError:
      if (t < 1800) {
        write((t % 600) < 300, false);
      } else {
        pattern_ = Pattern::kIdle;
        write(false, false);
      }
      break;
    case Pattern::kIdle:
      write(static_cast<int32_t>(blip_until_ms_ - now_ms) > 0, false);
      break;
  }
}

inline void AlertLed::write(bool led_on, bool buzzer_on) {
  if (led_pin_ >= 0) digitalWrite(led_pin_, led_on ? HIGH : LOW);
  if (buzzer_pin_ >= 0) digitalWrite(buzzer_pin_, buzzer_on ? HIGH : LOW);
}
