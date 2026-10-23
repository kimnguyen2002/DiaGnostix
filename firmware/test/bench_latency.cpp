// Host timing of the two stages of the ECG risk engine (ecg_risk.h), for the result-latency analysis:
//   stage 1  analyzeWindow : filtering, R peaks, delineation, features of one 10-s window
//   stage 2  finalize      : model scores, median, calibration, tier, borderline flag
// Batch evaluation runs stage 1 for every window after the recording; the firmware runs it for each window
// while the next one is recorded, so only the last window and stage 2 remain when the recording ends.
//
//   make -C firmware/test bench
// Times are of the host CPU; the firmware prints its own per-window time at boot (runSelfTest).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "dgx_selftest_vector.h"
#include "ecg_risk.h"

using namespace dgx;
using Clock = std::chrono::steady_clock;

template <typename F>
static double medianUs(int reps, F&& f) {
  std::vector<double> t(reps);
  for (int i = 0; i < reps; ++i) {
    const auto a = Clock::now();
    f();
    t[i] = std::chrono::duration<double, std::micro>(Clock::now() - a).count();
  }
  std::nth_element(t.begin(), t.begin() + reps / 2, t.end());
  return t[reps / 2];
}

int main() {
  static EcgRiskEngine engine;
  if (!engine.begin()) return 1;
  std::vector<int16_t> buf(kMaxWindows * kWindowSamples);
  for (int w = 0; w < kMaxWindows; ++w) std::copy(DGX_SELFTEST_MV, DGX_SELFTEST_MV + kWindowSamples, buf.begin() + w * kWindowSamples);
  NonEcgInputs in;
  in.age = 55; in.male_sex = 1; in.bmi = 24.5;

  static RiskEstimate est;
  for (int i = 0; i < 2000; ++i)  // warm-up: let the CPU reach its steady clock before timing
    engine.analyzeWindow(buf.data(), DGX_SELFTEST_GAIN, DGX_SELFTEST_REF_MV, false, 3100, est.window[0]);
  const double t_win = medianUs(200, [&] {
    engine.analyzeWindow(buf.data(), DGX_SELFTEST_GAIN, DGX_SELFTEST_REF_MV, false, 3100, est.window[0]);
  });
  for (int w = 1; w < kMaxWindows; ++w) est.window[w] = est.window[0];
  est.n_windows = 3;
  const double t_fin = medianUs(20000, [&] { EcgRiskEngine::finalize(ModelRole::kPrimary, in, 2, est); });

  std::printf("stage,us\nanalyze_window,%.1f\nfinalize,%.2f\n", t_win, t_fin);
  for (int n : {3, 6}) {
    const double t_batch = medianUs(50, [&] {
      engine.evaluate(buf.data(), n, DGX_SELFTEST_GAIN, DGX_SELFTEST_REF_MV, false, 3100, ModelRole::kPrimary, in, 2, est);
    });
    std::printf("batch_%d_windows,%.1f\nincremental_%d_windows,%.1f\n", n, t_batch, n, t_win + t_fin);
  }
  return 0;
}
