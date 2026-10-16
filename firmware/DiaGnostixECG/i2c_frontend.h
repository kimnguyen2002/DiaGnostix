#pragma once

#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <MAX30105.h>
#include <Wire.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "config.h"
#include "ppg_processor.h"

// MAX30102 pulse oximeter and BME280 environment sensor on the shared I2C
// bus. One task owns the bus: it drains the MAX30102 FIFO every 5 ms,
// reconstructs a sample timestamp for each FIFO entry, runs the PPG
// processor, and reads the BME280 every ENV_READ_INTERVAL_MS.
class I2cFrontEnd {
 public:
  struct ArrivalEvent {
    int64_t arrival_us;
  };

  struct Environment {
    float temp_c = NAN;
    float humidity = NAN;
    float pressure_hpa = NAN;
  };

  bool begin();

  bool ppgPresent() const { return ppg_ok_; }
  bool envPresent() const { return env_ok_; }

  bool receiveArrival(ArrivalEvent* ev) { return xQueueReceive(arrival_queue_, ev, 0) == pdTRUE; }
  size_t readWaveform(int16_t* out, size_t max_samples);
  bool fingerOn() const { return finger_.load(); }
  float spo2() const { return spo2_.load(); }
  float perfusionIndex() const { return pi_.load(); }
  float heartRateBpm() const { return hr_bpm_.load(); }
  Environment environment() const;

 private:
  static constexpr uint8_t kMax30102Address = 0x57;
  static constexpr uint8_t kRegFifoData = 0x07;
  static constexpr int kBytesPerSample = 6;      // red + IR, 3 bytes each
  static constexpr int kMaxSamplesPerRead = 16;  // 96 bytes, within the 128-byte Wire buffer
  static constexpr uint32_t kTaskStack = 6144;
  static constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 3;
  static constexpr int64_t kSamplePeriodUs = 1000000 / PPG_SAMPLE_RATE_HZ;

  static void taskEntry(void* self) { static_cast<I2cFrontEnd*>(self)->run(); }
  void run();
  int readFifo(uint32_t* red, uint32_t* ir, int max_samples);
  static uint32_t read18bit();
  void readEnvironment();

  MAX30105 max30102_;
  Adafruit_BME280 bme280_;
  bool ppg_ok_ = false;
  bool env_ok_ = false;

  dsp::PpgProcessor ppg_;
  QueueHandle_t arrival_queue_ = nullptr;
  QueueHandle_t wave_queue_ = nullptr;
  std::atomic<bool> finger_{false};
  std::atomic<float> spo2_{NAN};
  std::atomic<float> pi_{NAN};
  std::atomic<float> hr_bpm_{NAN};

  mutable portMUX_TYPE env_lock_ = portMUX_INITIALIZER_UNLOCKED;
  Environment env_;

  // Sample clock: time assigned to the next FIFO sample.
  int64_t next_sample_us_ = 0;
  bool clock_valid_ = false;
};

inline bool I2cFrontEnd::begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQUENCY_HZ);

  ppg_ok_ = max30102_.begin(Wire, I2C_FREQUENCY_HZ, kMax30102Address);
  if (ppg_ok_) {
    // ledBrightness, sampleAverage, ledMode (2 = red + IR), sampleRate, pulseWidth, adcRange
    max30102_.setup(PPG_LED_CURRENT, 4, 2, PPG_SAMPLE_RATE_HZ * 4, 215, 16384);
    max30102_.clearFIFO();
  }

  env_ok_ = bme280_.begin(0x76, &Wire) || bme280_.begin(0x77, &Wire);
  if (env_ok_) {
    // Forced mode with 1x oversampling: one conversion per read keeps
    // self-heating of the sensor negligible.
    bme280_.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1,
                        Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                        Adafruit_BME280::FILTER_OFF);
  }

  dsp::PpgConfig pcfg;
  pcfg.sample_rate_hz = PPG_SAMPLE_RATE_HZ;
  pcfg.finger_ir_threshold = PPG_FINGER_THRESHOLD;
  ppg_ = dsp::PpgProcessor(pcfg);

  arrival_queue_ = xQueueCreate(16, sizeof(ArrivalEvent));
  wave_queue_ = xQueueCreate(PPG_SAMPLE_RATE_HZ / PPG_STREAM_DECIMATION, sizeof(int16_t));
  if (!arrival_queue_ || !wave_queue_) return false;
  return xTaskCreatePinnedToCore(taskEntry, "i2c", kTaskStack, this, kTaskPriority, nullptr, 1) == pdPASS;
}

inline size_t I2cFrontEnd::readWaveform(int16_t* out, size_t max_samples) {
  size_t n = 0;
  while (n < max_samples && xQueueReceive(wave_queue_, &out[n], 0) == pdTRUE) ++n;
  return n;
}

inline I2cFrontEnd::Environment I2cFrontEnd::environment() const {
  portENTER_CRITICAL(&env_lock_);
  Environment e = env_;
  portEXIT_CRITICAL(&env_lock_);
  return e;
}

inline void I2cFrontEnd::run() {
  uint32_t red[kMaxSamplesPerRead];
  uint32_t ir[kMaxSamplesPerRead];
  uint32_t last_env_ms = 0;
  uint32_t sample_count = 0;
  int empty_polls = 0;

  for (;;) {
    if (ppg_ok_) {
      const int n = readFifo(red, ir, kMaxSamplesPerRead);
      const int64_t now = esp_timer_get_time();
      if (n == 0) {
        // With roll-over enabled a full FIFO reads as empty; recover from
        // stalls (e.g. a long Wi-Fi interrupt) by clearing it.
        if (++empty_polls > 20) {
          max30102_.clearFIFO();
          clock_valid_ = false;
          empty_polls = 0;
        }
      } else {
        empty_polls = 0;
        // Newest sample was taken within the last sample period. Keep a
        // smooth sample clock and steer it slowly towards the read time.
        if (!clock_valid_ || n >= kMaxSamplesPerRead) {
          next_sample_us_ = now - (n - 1) * kSamplePeriodUs - kSamplePeriodUs / 2;
          clock_valid_ = true;
        }
        for (int i = 0; i < n; ++i) {
          const int64_t t = next_sample_us_;
          next_sample_us_ += kSamplePeriodUs;
          dsp::PpgPulse pulse;
          if (ppg_.process(red[i], ir[i], t, &pulse)) {
            ArrivalEvent ev{pulse.arrival_us};
            xQueueSend(arrival_queue_, &ev, 0);
            hr_bpm_.store(ppg_.heartRateBpm());
          }
          if (++sample_count % PPG_STREAM_DECIMATION == 0 && ppg_.settled()) {
            const float w = std::max(-32000.0f, std::min(32000.0f, ppg_.waveform()));
            const int16_t v = static_cast<int16_t>(w);
            xQueueSend(wave_queue_, &v, 0);
          }
        }
        const int64_t newest = next_sample_us_ - kSamplePeriodUs;
        const int64_t err = (now - kSamplePeriodUs / 2) - newest;
        next_sample_us_ += err / 32;

        finger_.store(ppg_.fingerPresent());
        dsp::Spo2Estimate est;
        if (ppg_.spo2(&est)) {
          spo2_.store(est.spo2);
          pi_.store(est.perfusion_index);
        } else {
          spo2_.store(NAN);
          pi_.store(NAN);
        }
        if (!ppg_.fingerPresent()) hr_bpm_.store(NAN);
      }
    }

    if (env_ok_ && millis() - last_env_ms >= ENV_READ_INTERVAL_MS) {
      last_env_ms = millis();
      readEnvironment();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

inline int I2cFrontEnd::readFifo(uint32_t* red, uint32_t* ir, int max_samples) {
  const uint8_t wr = max30102_.getWritePointer();
  const uint8_t rd = max30102_.getReadPointer();
  int n = (wr - rd) & 0x1F;
  if (n == 0) return 0;
  n = std::min(n, max_samples);

  Wire.beginTransmission(kMax30102Address);
  Wire.write(kRegFifoData);
  if (Wire.endTransmission(false) != 0) return 0;
  const int bytes = n * kBytesPerSample;
  if (Wire.requestFrom(static_cast<int>(kMax30102Address), bytes) != bytes) return 0;
  for (int i = 0; i < n; ++i) {
    red[i] = read18bit();
    ir[i] = read18bit();
  }
  return n;
}

inline uint32_t I2cFrontEnd::read18bit() {
  // Separate statements: the evaluation order of operands of | is unspecified.
  const uint32_t b2 = Wire.read();
  const uint32_t b1 = Wire.read();
  const uint32_t b0 = Wire.read();
  return ((b2 << 16) | (b1 << 8) | b0) & 0x3FFFF;
}

inline void I2cFrontEnd::readEnvironment() {
  if (!bme280_.takeForcedMeasurement()) return;
  Environment e;
  e.temp_c = bme280_.readTemperature();
  e.humidity = bme280_.readHumidity();
  e.pressure_hpa = bme280_.readPressure() / 100.0f;
  portENTER_CRITICAL(&env_lock_);
  env_ = e;
  portEXIT_CRITICAL(&env_lock_);
}
