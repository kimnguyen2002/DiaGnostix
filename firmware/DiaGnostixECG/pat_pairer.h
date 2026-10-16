#pragma once

#include <cstdint>

#include "stats.h"

namespace dsp {

// Pairs every ECG R peak with the PPG pulse arrival that it caused and
// returns the pulse arrival time (PAT). R peaks and arrivals are reported by
// different detectors with different latencies, so pairing is deferred until
// all arrivals that could belong to a beat have been seen.
class PatPairer {
 public:
  struct Config {
    float min_pat_ms = 120.0f;
    float max_pat_ms = 450.0f;
    float settle_ms = 700.0f;  // wait this long after an R peak before pairing it
  };

  struct Pat {
    int64_t r_time_us;  // R peak of the beat
    float pat_ms;
  };

  PatPairer() : PatPairer(Config()) {}
  explicit PatPairer(const Config& cfg) : cfg_(cfg) {}

  void addRPeak(int64_t t_us) { r_.push({t_us, false}); }
  void addArrival(int64_t t_us) { a_.push({t_us, false}); }
  void reset() {
    r_.clear();
    a_.clear();
  }

  // Pairs all R peaks older than settle_ms. Writes at most max_out results
  // and returns how many were written.
  int poll(int64_t now_us, Pat* out, int max_out) {
    int written = 0;
    const int64_t settle = static_cast<int64_t>(cfg_.settle_ms * 1000.0f);
    const int64_t lo = static_cast<int64_t>(cfg_.min_pat_ms * 1000.0f);
    const int64_t hi = static_cast<int64_t>(cfg_.max_pat_ms * 1000.0f);
    for (size_t i = 0; i < r_.size() && written < max_out; ++i) {
      Event& r = r_[i];
      if (r.used || now_us - r.t_us < settle) continue;
      r.used = true;
      // An arrival after the next R peak belongs to the next beat.
      const int64_t next_r = i + 1 < r_.size() ? r_[i + 1].t_us : INT64_MAX;
      for (size_t j = 0; j < a_.size(); ++j) {
        Event& a = a_[j];
        if (a.used) continue;
        const int64_t dt = a.t_us - r.t_us;
        if (dt < lo) continue;
        if (dt > hi || a.t_us > next_r) break;
        a.used = true;
        out[written++] = {r.t_us, static_cast<float>(dt) / 1000.0f};
        break;
      }
    }
    return written;
  }

 private:
  struct Event {
    int64_t t_us;
    bool used;
  };

  Config cfg_;
  RingBuffer<Event, 16> r_;
  RingBuffer<Event, 16> a_;
};

}  // namespace dsp
