// Host unit tests for the signal-processing library. Run with:
//   make -C firmware/test
//
// A synthetic ECG (sum-of-Gaussians PQRST model with baseline wander, mains
// interference and white noise) and a synthetic PPG whose pulse arrival time
// and red/IR modulation ratio are known are fed through the same code that
// runs on the ESP32-S3, and the outputs are compared with the ground truth.

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "bp_estimator.h"
#include "pat_pairer.h"
#include "ppg_processor.h"
#include "qrs_detector.h"
#include "stats.h"
#include "thermistor.h"

using namespace dsp;

void setUp() {}
void tearDown() {}

namespace {

constexpr double kPi = 3.14159265358979323846;

struct SyntheticRecording {
  std::vector<double> r_times_s;        // ground-truth R apex times
  std::vector<double> arrival_times_s;  // ground-truth PPG max-slope times
  std::vector<double> pat_ms;           // ground-truth PAT per beat
};

struct SignalParams {
  double duration_s = 60.0;
  double mean_rr_s = 0.8;
  double ecg_noise = 15.0;  // ADC counts (R amplitude is 900)
  double pat_ms = 250.0;
  double spo2_ratio = 0.6;
  unsigned seed = 1;
  bool invert_ecg = false;
};

SyntheticRecording makeBeats(const SignalParams& p) {
  SyntheticRecording rec;
  std::mt19937 rng(p.seed);
  std::normal_distribution<double> jitter(0.0, 0.02);
  double t = 0.5;
  while (t < p.duration_s - 1.0) {
    rec.r_times_s.push_back(t);
    const double pat = p.pat_ms + 10.0 * std::sin(2 * kPi * t / 20.0);
    rec.pat_ms.push_back(pat);
    rec.arrival_times_s.push_back(t + pat / 1000.0);
    // Respiratory sinus arrhythmia (0.25 Hz) plus random variability.
    t += p.mean_rr_s * (1.0 + 0.05 * std::sin(2 * kPi * 0.25 * t) + jitter(rng));
  }
  return rec;
}

double ecgAt(double t, const SyntheticRecording& rec) {
  struct Wave { double a, mu, sigma; };
  static const Wave kWaves[] = {{0.12, -0.200, 0.025}, {-0.10, -0.030, 0.010}, {1.0, 0.0, 0.009},
                                {-0.25, 0.030, 0.010}, {0.30, 0.280, 0.045}};
  double v = 0.0;
  for (double tr : rec.r_times_s) {
    if (std::fabs(t - tr) > 0.6) continue;
    for (const Wave& w : kWaves) {
      const double z = (t - tr - w.mu) / w.sigma;
      v += w.a * std::exp(-0.5 * z * z);
    }
  }
  return v;
}

double logistic(double z) { return 1.0 / (1.0 + std::exp(-z)); }

// Blood-volume pulse whose maximum upstroke slope is exactly at the arrival.
double pulseAt(double t, const SyntheticRecording& rec) {
  double v = 0.0;
  for (double ta : rec.arrival_times_s) {
    const double dt = t - ta;
    if (dt < -0.2 || dt > 1.5) continue;
    const double up = logistic(dt / 0.015);
    const double decay = dt > 0 ? std::exp(-dt / 0.35) : 1.0;
    const double notch = 0.15 * std::exp(-0.5 * std::pow((dt - 0.28) / 0.04, 2));
    v += up * decay + notch;
  }
  return v;
}

struct RunResult {
  std::vector<int64_t> beats_us;
  std::vector<int64_t> arrivals_us;
  std::vector<float> pats_ms;
  float ecg_hr = NAN;
  float ppg_hr = NAN;
  Spo2Estimate spo2;
  bool spo2_valid = false;
};

RunResult run(const SignalParams& p, const SyntheticRecording& rec) {
  std::mt19937 rng(p.seed + 100);
  std::normal_distribution<double> ecg_noise(0.0, p.ecg_noise);
  std::normal_distribution<double> ppg_noise(0.0, 20.0);

  QrsDetectorConfig qcfg;
  qcfg.invert = p.invert_ecg;
  QrsDetector qrs(qcfg);
  PpgProcessor ppg;
  PatPairer pairer;
  RunResult out;

  const double ir_dc = 100000.0, red_dc = 60000.0, ir_ac = 1500.0;
  const double red_ac = p.spo2_ratio * ir_ac * red_dc / ir_dc;

  for (int64_t t_us = 0; t_us < static_cast<int64_t>(p.duration_s * 1e6); t_us += 1000) {
    const double t = t_us / 1e6;
    if (t_us % 2000 == 0) {  // ECG at 500 Hz
      double v = 900.0 * ecgAt(t, rec);
      if (p.invert_ecg) v = -v;
      v += 2048.0 + 150.0 * std::sin(2 * kPi * 0.25 * t) + 30.0 * std::sin(2 * kPi * 50.0 * t) +
           ecg_noise(rng);
      QrsBeat beat;
      if (qrs.process(static_cast<float>(v), t_us, &beat)) {
        out.beats_us.push_back(beat.r_time_us);
        pairer.addRPeak(beat.r_time_us);
      }
    }
    if (t_us % 5000 == 0) {  // PPG at 200 Hz
      const double pulse = pulseAt(t, rec);
      const double ir = ir_dc - ir_ac * pulse + ppg_noise(rng);
      const double red = red_dc - red_ac * pulse + ppg_noise(rng) * 0.6;
      PpgPulse pp;
      if (ppg.process(static_cast<uint32_t>(red), static_cast<uint32_t>(ir), t_us, &pp)) {
        out.arrivals_us.push_back(pp.arrival_us);
        pairer.addArrival(pp.arrival_us);
      }
    }
    if (t_us % 100000 == 0) {
      PatPairer::Pat pats[8];
      const int n = pairer.poll(t_us, pats, 8);
      for (int i = 0; i < n; ++i) out.pats_ms.push_back(pats[i].pat_ms);
    }
  }
  out.ecg_hr = qrs.heartRateBpm();
  out.ppg_hr = ppg.heartRateBpm();
  out.spo2_valid = ppg.spo2(&out.spo2);
  return out;
}

// Fraction of true events (after warm-up) matched by a detection within
// tol_ms, mean absolute timing error, and number of unmatched detections.
struct MatchStats {
  double sensitivity;
  double mean_abs_err_ms;
  int false_positives;
};

MatchStats match(const std::vector<double>& truth_s, const std::vector<int64_t>& det_us, double warmup_s,
                 double end_s, double tol_ms) {
  int tp = 0, total = 0;
  double err = 0;
  std::vector<bool> used(det_us.size(), false);
  for (double tt : truth_s) {
    if (tt < warmup_s || tt > end_s) continue;
    ++total;
    for (size_t i = 0; i < det_us.size(); ++i) {
      const double d_ms = det_us[i] / 1000.0 - tt * 1000.0;
      if (!used[i] && std::fabs(d_ms) <= tol_ms) {
        used[i] = true;
        ++tp;
        err += std::fabs(d_ms);
        break;
      }
    }
  }
  int fp = 0;
  for (size_t i = 0; i < det_us.size(); ++i) {
    const double ts = det_us[i] / 1e6;
    if (!used[i] && ts >= warmup_s && ts <= end_s) ++fp;
  }
  return {total ? double(tp) / total : 0.0, tp ? err / tp : 1e9, fp};
}

double meanHrBpm(const SyntheticRecording& rec) {
  return 60.0 * (rec.r_times_s.size() - 1) / (rec.r_times_s.back() - rec.r_times_s.front());
}

}  // namespace

// ---------------------------------------------------------------------------

void test_qrs_detects_beats_with_ms_timing() {
  SignalParams p;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  auto m = match(rec.r_times_s, r.beats_us, 3.0, p.duration_s - 2.0, 15.0);
  TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(0.99, m.sensitivity);
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, m.false_positives);
  TEST_ASSERT_LESS_THAN_DOUBLE(4.0, m.mean_abs_err_ms);
  TEST_ASSERT_FLOAT_WITHIN(4.0f, static_cast<float>(meanHrBpm(rec)), r.ecg_hr);
}

void test_qrs_tolerates_heavy_noise() {
  SignalParams p;
  p.ecg_noise = 45.0;
  p.seed = 7;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  auto m = match(rec.r_times_s, r.beats_us, 3.0, p.duration_s - 2.0, 20.0);
  TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(0.97, m.sensitivity);
  TEST_ASSERT_LESS_OR_EQUAL_INT(2, m.false_positives);
}

void test_qrs_tachycardia() {
  SignalParams p;
  p.mean_rr_s = 0.45;  // ~133 bpm
  p.pat_ms = 200.0;
  p.seed = 3;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  auto m = match(rec.r_times_s, r.beats_us, 3.0, p.duration_s - 2.0, 15.0);
  TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(0.98, m.sensitivity);
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, m.false_positives);
}

void test_qrs_inverted_leads() {
  SignalParams p;
  p.invert_ecg = true;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  auto m = match(rec.r_times_s, r.beats_us, 3.0, p.duration_s - 2.0, 15.0);
  TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(0.99, m.sensitivity);
}

void test_ppg_arrival_timing() {
  SignalParams p;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  auto m = match(rec.arrival_times_s, r.arrivals_us, 3.0, p.duration_s - 2.0, 10.0);
  TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(0.95, m.sensitivity);
  TEST_ASSERT_LESS_OR_EQUAL_INT(2, m.false_positives);
  TEST_ASSERT_LESS_THAN_DOUBLE(5.0, m.mean_abs_err_ms);
  TEST_ASSERT_FLOAT_WITHIN(4.0f, static_cast<float>(meanHrBpm(rec)), r.ppg_hr);
}

void test_pat_matches_ground_truth() {
  SignalParams p;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(static_cast<int>(0.8 * rec.r_times_s.size()) - 5,
                                   static_cast<int>(r.pats_ms.size()));
  std::vector<float> truth(rec.pat_ms.begin(), rec.pat_ms.end());
  std::vector<float> scratch(std::max(truth.size(), r.pats_ms.size()));
  const float med_true = median(truth.data(), truth.size(), scratch.data());
  const float med_est = median(r.pats_ms.data(), r.pats_ms.size(), scratch.data());
  TEST_ASSERT_FLOAT_WITHIN(5.0f, med_true, med_est);
}

void test_spo2_ratio_of_ratios() {
  SignalParams p;
  p.spo2_ratio = 0.6;
  auto rec = makeBeats(p);
  auto r = run(p, rec);
  TEST_ASSERT_TRUE(r.spo2_valid);
  const float expected = -45.060f * 0.36f + 30.354f * 0.6f + 94.845f;  // 96.8 %
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.6f, r.spo2.ratio);
  TEST_ASSERT_FLOAT_WITHIN(1.5f, expected, r.spo2.spo2);

  p.spo2_ratio = 0.9;  // hypoxaemia, about 85 %
  rec = makeBeats(p);
  r = run(p, rec);
  TEST_ASSERT_TRUE(r.spo2_valid);
  TEST_ASSERT_FLOAT_WITHIN(1.5f, -45.060f * 0.81f + 30.354f * 0.9f + 94.845f, r.spo2.spo2);
}

void test_ppg_requires_finger() {
  PpgProcessor ppg;
  PpgPulse pulse;
  int pulses = 0;
  for (int i = 0; i < 2000; ++i) {
    const uint32_t ir = 2000 + static_cast<uint32_t>(500 * std::sin(i * 0.03));
    pulses += ppg.process(1500, ir, i * 5000LL, &pulse);
  }
  TEST_ASSERT_FALSE(ppg.fingerPresent());
  TEST_ASSERT_EQUAL_INT(0, pulses);
}

void test_pairer_rejects_out_of_window() {
  PatPairer pairer;
  pairer.addRPeak(1000000);
  pairer.addArrival(1050000);  // 50 ms: too early, not a pulse of this beat
  pairer.addArrival(1260000);  // 260 ms: valid
  pairer.addRPeak(1900000);
  pairer.addArrival(2500000);  // 600 ms: too late
  PatPairer::Pat pats[4];
  TEST_ASSERT_EQUAL_INT(1, pairer.poll(3000000, pats, 4));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 260.0f, pats[0].pat_ms);
  TEST_ASSERT_TRUE(pats[0].r_time_us == 1000000);
  TEST_ASSERT_EQUAL_INT(0, pairer.poll(4000000, pats, 4));
}

void test_bp_calibration_round_trip_and_direction() {
  BpEstimator bp;
  BpCalibration cal;
  cal.sbp0_mmHg = 118.0f;
  cal.dbp0_mmHg = 76.0f;
  cal.pat0_ms = 240.0f;
  cal.calibrated = true;
  bp.setCalibration(cal);

  auto at_cal = bp.estimate(240.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 118.0f, at_cal.sbp_mmHg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 76.0f, at_cal.dbp_mmHg);

  auto faster = bp.estimate(216.0f);  // PAT 10 % shorter -> stiffer, higher pressure
  auto slower = bp.estimate(264.0f);
  TEST_ASSERT_TRUE(faster.sbp_mmHg > at_cal.sbp_mmHg + 8.0f);
  TEST_ASSERT_TRUE(faster.dbp_mmHg > at_cal.dbp_mmHg);
  TEST_ASSERT_TRUE(slower.sbp_mmHg < at_cal.sbp_mmHg - 8.0f);
  TEST_ASSERT_TRUE(faster.sbp_mmHg > faster.dbp_mmHg + 10.0f);
  TEST_ASSERT_TRUE(std::isnan(bp.estimate(0.0f).sbp_mmHg));
}

void test_thermistor_beta_equation() {
  ThermistorConfig c;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, thermistorCelsius(10000.0f, c));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 50.0f, thermistorCelsius(3588.0f, c));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, thermistorCelsiusFromMv(1650.0f, c));
  // Warmer -> lower NTC resistance -> lower voltage on the low side.
  TEST_ASSERT_TRUE(thermistorCelsiusFromMv(1200.0f, c) > 36.0f);
  TEST_ASSERT_TRUE(std::isnan(thermistorCelsiusFromMv(0.0f, c)));
  c.ntc_on_low_side = false;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, thermistorCelsiusFromMv(1650.0f, c));
}

void test_median_and_hrv() {
  float v[] = {5, 1, 4, 2, 3};
  float s[5];
  TEST_ASSERT_EQUAL_FLOAT(3.0f, median(v, 5, s));
  TEST_ASSERT_EQUAL_FLOAT(3.0f, median(v, 4, s));  // {5,1,4,2} -> (2 + 4) / 2
  float rr[] = {800, 820, 790, 810, 800};
  float sdnn, rmssd;
  hrvTimeDomain(rr, 5, &sdnn, &rmssd);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 11.40f, sdnn);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 21.21f, rmssd);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_qrs_detects_beats_with_ms_timing);
  RUN_TEST(test_qrs_tolerates_heavy_noise);
  RUN_TEST(test_qrs_tachycardia);
  RUN_TEST(test_qrs_inverted_leads);
  RUN_TEST(test_ppg_arrival_timing);
  RUN_TEST(test_pat_matches_ground_truth);
  RUN_TEST(test_spo2_ratio_of_ratios);
  RUN_TEST(test_ppg_requires_finger);
  RUN_TEST(test_pairer_rejects_out_of_window);
  RUN_TEST(test_bp_calibration_round_trip_and_direction);
  RUN_TEST(test_thermistor_beta_equation);
  RUN_TEST(test_median_and_hrv);
  return UNITY_END();
}
