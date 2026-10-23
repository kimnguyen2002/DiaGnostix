#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>

#include "config.h"
#include "ecg_risk.h"

// Background analysis of the ECG windows while the recording continues (low result latency).
//
// The ECG task fills the capture buffer; loop() posts window k as soon as its last sample has been
// captured; this task runs the expensive part (filtering, R peaks, delineation, features: stage 1 of
// ecg_risk.h) on it while window k+1 is being recorded. When the recording ends only the last window
// is left, and loop() then finalises the result in microseconds (stage 2).
//
// The task runs on core 0 at low priority: it yields to the Wi-Fi stack and never delays the 500 Hz
// sampling task or loop(), which run on core 1. A session generation number discards the output of
// a window that belongs to a cancelled session.
class AnalysisWorker {
 public:
  bool begin(dgx::EcgRiskEngine* engine) {
    engine_ = engine;
    jobs_ = xQueueCreate(dgx::kMaxWindows, sizeof(Job));
    lock_ = xSemaphoreCreateMutex();
    return jobs_ && lock_ && xTaskCreatePinnedToCore(taskEntry, "analysis", kStack, this, 1, nullptr, 0) == pdPASS;
  }

  // Starts a new session whose windows are written to est->window[].
  void startSession(dgx::RiskEstimate* est, double gain, bool invert) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    gen_.fetch_add(1);
    xQueueReset(jobs_);
    est_ = est;
    gain_ = gain;
    invert_ = invert;
    done_.store(0);
    last_window_us_.store(0);
    xSemaphoreGive(lock_);
  }

  bool post(int index, const int16_t* samples) {
    Job j{gen_.load(), index, samples};
    return xQueueSend(jobs_, &j, 0) == pdTRUE;
  }

  int done() const { return done_.load(std::memory_order_acquire); }
  uint32_t lastWindowUs() const { return last_window_us_.load(); }

 private:
  struct Job {
    uint32_t gen;
    int index;
    const int16_t* samples;
  };
  static constexpr uint32_t kStack = 8192;

  static void taskEntry(void* self) { static_cast<AnalysisWorker*>(self)->run(); }

  void run() {
    Job j;
    for (;;) {
      if (xQueueReceive(jobs_, &j, portMAX_DELAY) != pdTRUE) continue;
      const int64_t t0 = esp_timer_get_time();
      engine_->analyzeWindow(j.samples, gain_, ECG_ADC_REF_MV, invert_, ECG_ADC_MAX_MV, wr_);
      xSemaphoreTake(lock_, portMAX_DELAY);
      if (j.gen == gen_.load()) {  // otherwise the session was cancelled or restarted meanwhile
        est_->window[j.index] = wr_;
        last_window_us_.store(static_cast<uint32_t>(esp_timer_get_time() - t0));
        done_.fetch_add(1, std::memory_order_release);
      }
      xSemaphoreGive(lock_);
    }
  }

  dgx::EcgRiskEngine* engine_ = nullptr;
  dgx::RiskEstimate* est_ = nullptr;
  QueueHandle_t jobs_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  dgx::WindowResult wr_;  // working copy (kept off the task stack)
  double gain_ = ECG_NOMINAL_GAIN;
  bool invert_ = false;
  std::atomic<uint32_t> gen_{0};
  std::atomic<int> done_{0};
  std::atomic<uint32_t> last_window_us_{0};
};
