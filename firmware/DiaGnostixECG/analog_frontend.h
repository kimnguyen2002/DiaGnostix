#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cmath>

#include "config.h"
#include "ecg_risk.h"
#include "qrs_detector.h"
#include "thermistor.h"

static_assert(configTICK_RATE_HZ >= ECG_SAMPLE_RATE_HZ, "FreeRTOS tick too slow for ECG sampling");
static_assert(ECG_SAMPLE_RATE_HZ == dgx::kFs, "the risk models were trained on 500 Hz ECG features");

// AD8232 ECG and NTC thermistor, both on ADC1. A dedicated task on core 1
// samples the ECG every 2 ms (500 Hz, crystal-timed FreeRTOS tick) with
// analogReadMilliVolts(), runs live QRS detection, streams a decimated
// waveform and, while a capture is armed, records the raw millivolts that the
// risk model analyses (ecg_risk.h). Lead-off samples are marked, not dropped,
// so the windows keep their timing.
class AnalogFrontEnd {
 public:
  struct BeatEvent {
    int64_t r_time_us;
    float rr_ms;  // 0 if unknown
  };

  bool begin();

  // Thread-safe accessors for the main loop.
  bool receiveBeat(BeatEvent* ev) { return xQueueReceive(beat_queue_, ev, 0) == pdTRUE; }
  size_t readWaveform(int16_t* out, size_t max_samples);
  bool leadsOn() const { return leads_on_.load(); }
  float heartRateBpm() const { return hr_bpm_.load(); }
  float bodyTemperatureC() const { return body_temp_c_.load(); }

  // Records n samples into buf after skipping skip samples (warm-up). The
  // buffer must stay valid until captureDone() or stopCapture().
  void startCapture(int16_t* buf, size_t n, size_t skip);
  void stopCapture() { capture_armed_.store(false); }
  size_t captured() const { return cap_count_.load(); }
  bool captureDone() const { return cap_count_.load() >= cap_n_; }

 private:
  static constexpr uint32_t kTaskStack = 4096;
  static constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 2;
  static constexpr int kThermistorEverySamples = ECG_SAMPLE_RATE_HZ / 4;  // 4 Hz
  static constexpr int kThermistorOversample = 8;

  static void taskEntry(void* self) { static_cast<AnalogFrontEnd*>(self)->run(); }
  void run();
  void record(bool on, int mv);
  void sampleThermistor();

  QueueHandle_t beat_queue_ = nullptr;
  QueueHandle_t wave_queue_ = nullptr;
  dsp::QrsDetector qrs_;
  dsp::ThermistorConfig ntc_;
  std::atomic<bool> leads_on_{false};
  std::atomic<float> hr_bpm_{NAN};
  std::atomic<float> body_temp_c_{NAN};

  // capture (written by the ECG task, read by loop())
  std::atomic<bool> capture_armed_{false};
  int16_t* cap_buf_ = nullptr;
  size_t cap_n_ = 0;
  size_t cap_skip_ = 0;
  std::atomic<size_t> cap_count_{0};
};

inline bool AnalogFrontEnd::begin() {
  pinMode(PIN_ECG_LO_PLUS, INPUT);
  pinMode(PIN_ECG_LO_MINUS, INPUT);
  if (PIN_ECG_SDN >= 0) {
    pinMode(PIN_ECG_SDN, OUTPUT);
    digitalWrite(PIN_ECG_SDN, HIGH);
  }
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ECG_OUTPUT, ADC_11db);  // 0-3.1 V range, as in the training simulation
  analogSetPinAttenuation(PIN_NTC, ADC_11db);

  dsp::QrsDetectorConfig qcfg;
  qcfg.sample_rate_hz = ECG_SAMPLE_RATE_HZ;
  qcfg.invert = ECG_INVERT;
  qrs_ = dsp::QrsDetector(qcfg);

  ntc_.series_ohm = NTC_SERIES_OHM;
  ntc_.nominal_ohm = NTC_NOMINAL_OHM;
  ntc_.beta = NTC_BETA;
  ntc_.ntc_on_low_side = NTC_ON_LOW_SIDE;

  beat_queue_ = xQueueCreate(16, sizeof(BeatEvent));
  wave_queue_ = xQueueCreate(ECG_SAMPLE_RATE_HZ / ECG_STREAM_DECIMATION, sizeof(int16_t));
  if (!beat_queue_ || !wave_queue_) return false;
  return xTaskCreatePinnedToCore(taskEntry, "ecg", kTaskStack, this, kTaskPriority, nullptr, 1) == pdPASS;
}

inline void AnalogFrontEnd::startCapture(int16_t* buf, size_t n, size_t skip) {
  capture_armed_.store(false);
  cap_buf_ = buf;
  cap_n_ = n;
  cap_skip_ = skip;
  cap_count_.store(0);
  capture_armed_.store(true, std::memory_order_release);
}

inline size_t AnalogFrontEnd::readWaveform(int16_t* out, size_t max_samples) {
  size_t n = 0;
  while (n < max_samples && xQueueReceive(wave_queue_, &out[n], 0) == pdTRUE) ++n;
  return n;
}

inline void AnalogFrontEnd::record(bool on, int mv) {
  if (!capture_armed_.load(std::memory_order_acquire)) return;
  if (cap_skip_ > 0) {
    --cap_skip_;
    return;
  }
  const size_t i = cap_count_.load();
  if (i >= cap_n_) {
    capture_armed_.store(false);
    return;
  }
  cap_buf_[i] = on ? static_cast<int16_t>(mv) : dgx::kLeadOffSample;
  cap_count_.store(i + 1);
}

inline void AnalogFrontEnd::run() {
  const TickType_t period = pdMS_TO_TICKS(1000 / ECG_SAMPLE_RATE_HZ);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t n = 0;
  bool was_on = false;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);
    const int64_t t_us = esp_timer_get_time();
    const bool on = digitalRead(PIN_ECG_LO_PLUS) == LOW && digitalRead(PIN_ECG_LO_MINUS) == LOW;
    const int mv = static_cast<int>(analogReadMilliVolts(PIN_ECG_OUTPUT));  // eFuse-calibrated millivolts
    ++n;
    record(on, mv);

    if (on != was_on) {
      qrs_.reset();  // restart threshold learning after (re)attachment
      leads_on_.store(on);
      if (!on) hr_bpm_.store(NAN);
      was_on = on;
    }

    if (on) {
      dsp::QrsBeat beat;
      if (qrs_.process(static_cast<float>(mv), t_us, &beat)) {  // adaptive thresholds: unit-free
        BeatEvent ev{beat.r_time_us, beat.rr_ms};
        xQueueSend(beat_queue_, &ev, 0);
        hr_bpm_.store(qrs_.heartRateBpm());
      }
      if (n % ECG_STREAM_DECIMATION == 0) {
        const int16_t v = static_cast<int16_t>(mv);
        xQueueSend(wave_queue_, &v, 0);  // dropped if nobody is reading
      }
    }

    if (n % kThermistorEverySamples == 0) sampleThermistor();
  }
}

inline void AnalogFrontEnd::sampleThermistor() {
  uint32_t mv = 0;
  for (int i = 0; i < kThermistorOversample; ++i) mv += analogReadMilliVolts(PIN_NTC);
  const float t = dsp::thermistorCelsiusFromMv(static_cast<float>(mv) / kThermistorOversample, ntc_);
  if (!std::isfinite(t) || t < -20.0f || t > 80.0f) {
    body_temp_c_.store(NAN);  // open or shorted thermistor
    return;
  }
  const float prev = body_temp_c_.load();
  body_temp_c_.store(std::isfinite(prev) ? prev + 0.1f * (t - prev) : t);  // ~2.5 s time constant
}
