// Host test and benchmark of the vital-sign smoother (smoothing.h). Run with:
//   make -C firmware/test            (tests)
//   ./test_smoothing --csv out.csv   (also writes the simulated streams for plotting)
//
// Each scenario is a 1-Hz reading stream of 600 s: a true value with a step change at t = 300 s,
// Gaussian sensor noise and sparse outlier spikes (missed or double beats, motion, I2C glitches).
// Metrics: jitter = RMS of successive differences of the displayed value; error = RMS deviation from
// the true value outside the 30 s after the step; worst error outside the step; 90 % step-response time.

#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "smoothing.h"

void setUp() {}
void tearDown() {}

namespace {

struct Scenario {
  const char* name;
  const char* unit;
  float before, after;   // true value before / after the step at t = 300 s
  float noise_sd;
  float outlier_rate;
  float outlier_size;    // spike amplitude (sign random)
  float quantum;         // sensor resolution (0 = none)
  dsp::VitalSmoother::Config cfg;
};

// Same settings as the firmware (DiaGnostixECG.ino, VitalFilters). The firmware smooths the pulse arrival time
// (outlier floor 8 ms) and derives BP from it; near PAT = 250 ms, 8 ms correspond to ~4 mmHg (dMAP/dPAT =
// -(2/gamma)/PAT, gamma = 0.017/mmHg), the floor used here for systolic BP.
const Scenario kScenarios[] = {
    {"Heart rate (ECG)", "bpm", 72.0f, 88.0f, 2.0f, 0.05f, 25.0f, 0.0f, dsp::vitalConfig(4.0f, 3.0f, 0.5f)},
    {"SpO2", "%", 97.0f, 93.0f, 1.0f, 0.05f, 8.0f, 1.0f, dsp::vitalConfig(5.0f, 1.5f, 0.25f)},
    {"Systolic BP (PAT)", "mmHg", 120.0f, 132.0f, 4.0f, 0.03f, 25.0f, 0.0f, dsp::vitalConfig(5.0f, 4.0f, 0.5f)},
    {"Skin temperature", "C", 33.5f, 34.5f, 0.08f, 0.01f, 2.0f, 0.0f, dsp::vitalConfig(5.0f, 0.3f, 0.05f)},
};

struct Metrics {
  double jitter_raw, jitter_out, err_raw, err_out, worst_raw, worst_out, step90_s;
  int outliers;
};

Metrics run(const Scenario& sc, unsigned seed, std::vector<float>* truth_out = nullptr,
            std::vector<float>* raw_out = nullptr, std::vector<float>* shown_out = nullptr) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, sc.noise_sd);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  dsp::VitalSmoother sm(sc.cfg);
  const int n = 600, t_step = 300;
  std::vector<float> truth(n), raw(n), out(n);
  for (int t = 0; t < n; ++t) {
    truth[t] = t < t_step ? sc.before : sc.after;
    float x = truth[t] + noise(rng);
    if (u(rng) < sc.outlier_rate) x += (u(rng) < 0.5f ? -1.0f : 1.0f) * sc.outlier_size;
    if (sc.quantum > 0) x = sc.quantum * std::round(x / sc.quantum);
    raw[t] = x;
    out[t] = sm.update(x, 1.0f);
  }
  Metrics m{};
  double sdr = 0, sdo = 0, er = 0, eo = 0;
  int nd = 0, ne = 0;
  for (int t = 10; t < n; ++t) {  // skip the start-up
    const bool near_step = t >= t_step && t < t_step + 30;
    if (t != t_step) {
      sdr += (raw[t] - raw[t - 1]) * (raw[t] - raw[t - 1]);
      sdo += (out[t] - out[t - 1]) * (out[t] - out[t - 1]);
      ++nd;
    }
    if (!near_step) {
      er += (raw[t] - truth[t]) * (raw[t] - truth[t]);
      eo += (out[t] - truth[t]) * (out[t] - truth[t]);
      m.worst_raw = std::max(m.worst_raw, (double)std::fabs(raw[t] - truth[t]));
      m.worst_out = std::max(m.worst_out, (double)std::fabs(out[t] - truth[t]));
      ++ne;
    }
  }
  m.jitter_raw = std::sqrt(sdr / nd);
  m.jitter_out = std::sqrt(sdo / nd);
  m.err_raw = std::sqrt(er / ne);
  m.err_out = std::sqrt(eo / ne);
  const float target = sc.before + 0.9f * (sc.after - sc.before);
  m.step90_s = NAN;
  for (int t = t_step; t < n; ++t) {
    const bool reached = sc.after > sc.before ? out[t] >= target : out[t] <= target;
    if (reached) {
      m.step90_s = t - t_step;
      break;
    }
  }
  m.outliers = sm.outliers();
  if (truth_out) *truth_out = truth;
  if (raw_out) *raw_out = raw;
  if (shown_out) *shown_out = out;
  return m;
}

// Average over 50 random seeds for the reported numbers.
Metrics averaged(const Scenario& sc) {
  Metrics s{};
  const int reps = 50;
  for (int r = 0; r < reps; ++r) {
    const Metrics m = run(sc, 1000 + r);
    s.jitter_raw += m.jitter_raw / reps;
    s.jitter_out += m.jitter_out / reps;
    s.err_raw += m.err_raw / reps;
    s.err_out += m.err_out / reps;
    s.worst_raw += m.worst_raw / reps;
    s.worst_out += m.worst_out / reps;
    s.step90_s += m.step90_s / reps;
  }
  return s;
}

}  // namespace

void test_smoother_reduces_jitter_and_error() {
  std::printf("  %-20s %8s %8s %7s %8s %8s %9s %9s %6s\n", "vital", "jit raw", "jit out", "jit -%", "err raw",
              "err out", "worst raw", "worst out", "t90 s");
  for (const Scenario& sc : kScenarios) {
    const Metrics m = averaged(sc);
    std::printf("  %-20s %8.3f %8.3f %6.1f%% %8.3f %8.3f %9.3f %9.3f %6.1f\n", sc.name, m.jitter_raw, m.jitter_out,
                100.0 * (1.0 - m.jitter_out / m.jitter_raw), m.err_raw, m.err_out, m.worst_raw, m.worst_out, m.step90_s);
    TEST_ASSERT_LESS_THAN_DOUBLE(0.35 * m.jitter_raw, m.jitter_out);  // >= 65 % less jitter
    TEST_ASSERT_LESS_THAN_DOUBLE(m.err_raw, m.err_out);               // closer to the true value
    TEST_ASSERT_LESS_THAN_DOUBLE(m.worst_raw, m.worst_out);          // spikes do not reach the display
    TEST_ASSERT_LESS_THAN_DOUBLE(20.0, m.step90_s);                  // real changes followed within 20 s
  }
}

void test_smoother_holds_then_clears_missing_readings() {
  dsp::VitalSmoother sm(dsp::vitalConfig(3.0f, 1.0f, 0.0f));
  for (int i = 0; i < 10; ++i) sm.update(70.0f, 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 70.0, sm.update(NAN, 1.0f));  // held
  sm.update(NAN, 1.0f);
  sm.update(NAN, 1.0f);
  TEST_ASSERT_TRUE(std::isnan(sm.update(NAN, 1.0f)));            // > hold_s (3 s): cleared
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 80.0, sm.update(80.0f, 1.0f));  // restarts from the first new reading
}

void test_smoother_rejects_single_spike() {
  dsp::VitalSmoother sm(dsp::vitalConfig(3.0f, 3.0f, 0.0f));
  for (int i = 0; i < 10; ++i) sm.update(70.0f, 1.0f);
  const float y = sm.update(140.0f, 1.0f);  // double-counted beats for one second
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 70.0, y);
  TEST_ASSERT_EQUAL_INT(1, sm.outliers());
}

void test_smoother_deadband_stops_flicker() {
  dsp::VitalSmoother sm(dsp::vitalConfig(1.0f, 10.0f, 0.5f));
  sm.update(70.0f, 1.0f);
  for (int i = 0; i < 20; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-4, 70.0, sm.update(i % 2 ? 70.4f : 69.6f, 1.0f));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_smoother_reduces_jitter_and_error);
  RUN_TEST(test_smoother_holds_then_clears_missing_readings);
  RUN_TEST(test_smoother_rejects_single_spike);
  RUN_TEST(test_smoother_deadband_stops_flicker);
  if (argc > 2 && !std::strcmp(argv[1], "--csv")) {  // streams + averaged metrics for the figures
    FILE* f = std::fopen(argv[2], "w");
    std::fprintf(f, "vital,unit,t_s,true,raw,shown\n");
    for (const Scenario& sc : kScenarios) {
      std::vector<float> truth, raw, shown;
      run(sc, 7, &truth, &raw, &shown);
      for (size_t t = 0; t < truth.size(); ++t)
        std::fprintf(f, "%s,%s,%zu,%.4f,%.4f,%.4f\n", sc.name, sc.unit, t, truth[t], raw[t], shown[t]);
    }
    std::fclose(f);
    std::string mpath = std::string(argv[2]) + ".metrics.csv";
    FILE* g = std::fopen(mpath.c_str(), "w");
    std::fprintf(g, "vital,unit,jitter_raw,jitter_out,err_raw,err_out,worst_raw,worst_out,step90_s,tau_s,min_dev,deadband\n");
    for (const Scenario& sc : kScenarios) {
      const Metrics m = averaged(sc);
      std::fprintf(g, "%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f\n", sc.name, sc.unit, m.jitter_raw,
                   m.jitter_out, m.err_raw, m.err_out, m.worst_raw, m.worst_out, m.step90_s, sc.cfg.tau_s, sc.cfg.min_dev,
                   sc.cfg.deadband);
    }
    std::fclose(g);
  }
  return UNITY_END();
}
