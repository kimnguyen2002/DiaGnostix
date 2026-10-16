// ecg_features.h - lead-I ECG feature extractor for one 10-s window at 500 Hz.
//
// C++ port of ml/dgx/edge_features.py (device_features), the reference the models were
// trained on. Every step is reproduced in the same order and in double precision, including numpy's
// summation order (pairwise sums, sequential cumulative sums, linear-interpolated percentile), so the
// host test in firmware/test matches the Python golden vectors to ~1e-12.
//
//   x [uV] -> subtract median -> zero-phase 0.5-40 Hz band-pass (2 biquads, forward + backward,
//   1 s odd-reflection padding) -> gradient/boxcar R-peak detector with T-wave rejection ->
//   fixed-window P/Q/S/T delineation -> 32 features + signal-quality index (SQI).
//
// Work buffers (~310 KB) live in PSRAM on the ESP32-S3 N16R8 (ps_malloc) and in the heap on a host.
// No Arduino dependency: the same file compiles in firmware/test with a desktop compiler.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "dgx_dsp_spec.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp32-hal-psram.h>
#endif

namespace dgx {

constexpr int kFs = 500;                        // Hz, ECG_SAMPLE_RATE_HZ in config.h
constexpr int kWindowSamples = 5000;            // 10 s
constexpr int kPad = 500;                       // int(PAD_S * fs)
constexpr int kMaxBeats = 64;                   // > 10 s / min R-R distance (0.3 s) + 1
constexpr int kMaxSegments = kWindowSamples / 2 + 1;
constexpr int kMaxBoxcar = 375;                 // round(AVG_S * fs)
constexpr int kNumFeatures = 32;

// Feature order = EDGE_ALL in edge_features.py = input names in the generated model headers.
enum FeatureIndex : uint8_t {
  F_MEAN_RR_S, F_SD_RR_S, F_MEAN_R_UV, F_SD_R_UV, F_MEAN_PP_S, F_SD_PP_S, F_MEAN_P_UV, F_SD_P_UV,
  F_MEAN_TT_S, F_SD_TT_S, F_MEAN_T_UV, F_SD_T_UV, F_MEAN_QQ_S, F_SD_QQ_S, F_MEAN_Q_UV, F_SD_Q_UV,
  F_MEAN_SS_S, F_SD_SS_S, F_MEAN_S_UV, F_SD_S_UV, F_QRS_MEAN_MS, F_QRS_SD_MS, F_RMSSD_MS,
  F_HR_SD_BPM, F_HR_MEDIAN_BPM, F_HR_MIN_BPM, F_HR_MAX_BPM, F_HR_MEAN_BPM,
  F_PR_PEAK_MS, F_QT_PEAK_MS, F_QTC_PEAK_MS, F_T_R_RATIO,
};

inline constexpr const char* kFeatureNames[kNumFeatures] = {
    "mean_rr_s", "sd_rr_s", "mean_r_uv", "sd_r_uv", "mean_pp_s", "sd_pp_s", "mean_p_uv", "sd_p_uv",
    "mean_tt_s", "sd_tt_s", "mean_t_uv", "sd_t_uv", "mean_qq_s", "sd_qq_s", "mean_q_uv", "sd_q_uv",
    "mean_ss_s", "sd_ss_s", "mean_s_uv", "sd_s_uv", "qrs_mean_ms", "qrs_sd_ms", "rmssd_ms",
    "hr_sd_bpm", "hr_median_bpm", "hr_min_bpm", "hr_max_bpm", "hr_mean_bpm",
    "pr_peak_ms", "qt_peak_ms", "qtc_peak_ms", "t_r_ratio"};

struct EcgFeatures {
  double value[kNumFeatures];  // NAN = not measurable in this window
  int n_beats = 0;
  double rr_ratio = NAN;       // longest / shortest R-R interval
  double beat_corr = NAN;      // mean beat-to-template correlation
  bool sqi_ok = false;         // window usable for a risk estimate
};

// ------------------------------------------------------------------ numpy-equivalent reductions
// np.add.reduce on a contiguous float64 array (pairwise summation, 8-way unrolled blocks of <= 128).
inline double np_sum(const double* a, int n) {
  if (n < 8) {
    double r = 0.0;
    for (int i = 0; i < n; ++i) r += a[i];
    return r;
  }
  if (n <= 128) {
    double r[8];
    for (int j = 0; j < 8; ++j) r[j] = a[j];
    int i = 8;
    for (; i < n - (n % 8); i += 8)
      for (int j = 0; j < 8; ++j) r[j] += a[i + j];
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  int n2 = n / 2;
  n2 -= n2 % 8;
  return np_sum(a, n2) + np_sum(a + n2, n - n2);
}

// a.mean() and a.std() (ddof = 0); sd is NAN for fewer than two values, both NAN when empty.
inline void mean_sd(const double* a, int n, double& mean, double& sd) {
  if (n <= 0) {
    mean = sd = NAN;
    return;
  }
  mean = np_sum(a, n) / n;
  if (n < 2) {
    sd = NAN;
    return;
  }
  double d[kMaxBeats];
  for (int i = 0; i < n; ++i) {
    double t = a[i] - mean;
    d[i] = t * t;
  }
  sd = sqrt(np_sum(d, n) / n);
}

inline double np_mean(const double* a, int n) { return n > 0 ? np_sum(a, n) / n : NAN; }

// np.median: middle value, or the mean of the two middle values. scratch holds n doubles.
inline double np_median(const double* a, int n, double* scratch) {
  memcpy(scratch, a, sizeof(double) * n);
  int h = n / 2;
  std::nth_element(scratch, scratch + h, scratch + n);
  double upper = scratch[h];
  if (n % 2) return upper;
  double lower = *std::max_element(scratch, scratch + h);
  return (lower + upper) / 2;
}

// np.percentile(a, q) with the default 'linear' method. scratch holds n doubles.
inline double np_percentile(const double* a, int n, double q, double* scratch) {
  memcpy(scratch, a, sizeof(double) * n);
  std::sort(scratch, scratch + n);
  double h = (n - 1) * (q / 100.0);
  int lo = (int)floor(h);
  int hi = std::min(lo + 1, n - 1);
  double t = h - lo, A = scratch[lo], B = scratch[hi], diff = B - A;
  return t >= 0.5 ? B - diff * (1 - t) : A + diff * t;
}

inline int argmax(const double* a, int n) { return (int)(std::max_element(a, a + n) - a); }
inline int argmin(const double* a, int n) { return (int)(std::min_element(a, a + n) - a); }
inline int win(double seconds) { return (int)nearbyint(seconds * kFs); }  // int(round(s * fs))

// scipy.signal.sosfilt with zero initial state, in place (Direct-Form II transposed per section).
inline void sosfilt_inplace(const double* sos, int n_sections, double* x, int n, bool reverse) {
  double zi[8][2] = {};
  for (int k = 0; k < n; ++k) {
    double* p = reverse ? &x[n - 1 - k] : &x[k];
    double x_cur = *p;
    for (int s = 0; s < n_sections; ++s) {
      const double* c = sos + 6 * s;
      double x_new = c[0] * x_cur + zi[s][0];
      zi[s][0] = c[1] * x_cur - c[4] * x_new + zi[s][1];
      zi[s][1] = c[2] * x_cur - c[5] * x_new;
      x_cur = x_new;
    }
    *p = x_cur;
  }
}

// ------------------------------------------------------------------ extractor
class EcgFeatureExtractor {
 public:
  // Doubles and ints allocated by begin(); reported in the boot log and used in the notebook budget.
  static constexpr size_t kWorkDoubles =
      (size_t)kWindowSamples * 4 + (kWindowSamples + 2 * kPad) + (kWindowSamples + kMaxBoxcar + 1) + 2 * kMaxSegments;
  static constexpr size_t kWorkBytes = kWorkDoubles * sizeof(double) + 2 * kMaxSegments * sizeof(int);

  bool begin() {
    if (block_) return true;
    block_ = alloc(kWorkBytes);
    if (!block_) return false;
    double* d = (double*)block_;
    in_ = d;            d += kWindowSamples;
    ext_ = d;           d += kWindowSamples + 2 * kPad;
    grad_ = d;          d += kWindowSamples;
    sm_ = d;            d += kWindowSamples;
    av_ = d;            d += kWindowSamples;
    cs_ = d;            d += kWindowSamples + kMaxBoxcar + 1;
    slopes_ = d;        d += kMaxSegments;
    scratch_ = d;       d += kMaxSegments;
    int* p = (int*)d;
    beg_ = p;           p += kMaxSegments;
    end_ = p;
    return true;
  }

  void end() {
    free(block_);
    block_ = nullptr;
  }

  bool ready() const { return block_ != nullptr; }

  // Fill this buffer with kWindowSamples samples in microvolts, then call compute().
  double* input() { return in_; }

  // Features of the n samples in input() (n = kWindowSamples for the trained models).
  bool compute(EcgFeatures& out, int n = kWindowSamples) {
    if (!block_ || n < 2 * kPad + 2 || n > kWindowSamples) return false;
    n_ = n;
    // 1. remove the median, zero-phase band-pass (odd reflection padding of 1 s at both ends)
    double med = np_median(in_, n, grad_);
    double* x = ext_ + kPad;
    for (int i = 0; i < n; ++i) x[i] = in_[i] - med;
    for (int i = 0; i < kPad; ++i) {
      ext_[i] = 2 * x[0] - x[kPad - i];                 // x[pad:0:-1]
      x[n + i] = 2 * x[n - 1] - x[n - 2 - i];           // x[-2:-pad-2:-1]
    }
    sosfilt_inplace(DGX_SOS_500HZ, kSections, ext_, n + 2 * kPad, false);
    sosfilt_inplace(DGX_SOS_500HZ, kSections, ext_, n + 2 * kPad, true);
    y_ = x;  // filtered signal = ext[pad:-pad]
    // 2. R peaks, 3. delineation, 4. features
    detect_r_peaks();
    delineate();
    compute_features(out);
    return true;
  }

  // Debug access (valid after compute): filtered signal and fiducial indices (-1 = not found).
  const double* filtered() const { return y_; }
  int beats() const { return nb_; }
  const int* r_peaks() const { return R_; }

 private:
  static constexpr int kSections = (int)(sizeof(DGX_SOS_500HZ) / sizeof(DGX_SOS_500HZ[0]) / 6);
  static_assert(kSections == 2, "dgx_dsp_spec.h: expected two biquads for the 500 Hz band-pass");

  static void* alloc(size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32)
    if (psramFound()) return ps_malloc(bytes);
#endif
    return malloc(bytes);
  }

  // Centred boxcar of length k with edge replication (cumulative-sum form, as in Python).
  void moving_average(const double* x, int k, double* out) {
    int n = n_;
    if (k <= 1) {
      memcpy(out, x, sizeof(double) * n);
      return;
    }
    int h = k / 2, m = n + k - 1;
    double acc = 0.0;
    cs_[0] = 0.0;
    for (int j = 0; j < m; ++j) {
      acc += (j < h) ? x[0] : (j < h + n ? x[j - h] : x[n - 1]);
      cs_[j + 1] = acc;
    }
    for (int i = 0; i < n; ++i) out[i] = (cs_[i + k] - cs_[i]) / k;
  }

  void detect_r_peaks() {
    const int n = n_;
    const double* y = y_;
    nb_ = 0;
    // |np.gradient(y)|
    grad_[0] = fabs((y[1] - y[0]) / 1.0);
    for (int i = 1; i < n - 1; ++i) grad_[i] = fabs((y[i + 1] - y[i - 1]) / 2.0);
    grad_[n - 1] = fabs((y[n - 1] - y[n - 2]) / 1.0);
    moving_average(grad_, win(DGX_SMOOTH_S), sm_);
    moving_average(sm_, win(DGX_AVG_S), av_);
    auto qrs = [&](int i) { return sm_[i] > DGX_THR_WEIGHT * av_[i]; };
    int nbeg = 0, nend = 0;
    for (int i = 0; i < n - 1; ++i) {
      bool a = qrs(i), b = qrs(i + 1);
      if (!a && b) beg_[nbeg++] = i;
      if (a && !b) end_[nend++] = i;
    }
    if (nbeg == 0) return;
    int k = 0;  // end = end[end > beg[0]]
    for (int i = 0; i < nend; ++i)
      if (end_[i] > beg_[0]) end_[k++] = end_[i];
    nend = k;
    int nseg = std::min(nbeg, nend);
    if (nseg == 0) return;
    for (int i = 0; i < nseg; ++i) scratch_[i] = (double)(end_[i] - beg_[i]);
    double min_len = DGX_MIN_LEN_WEIGHT * (np_sum(scratch_, nseg) / nseg);
    int ns = 0;  // keep segments of sufficient length (compacted in place)
    for (int i = 0; i < nseg; ++i) {
      if (end_[i] - beg_[i] >= min_len) {
        beg_[ns] = beg_[i];
        end_[ns] = end_[i];
        slopes_[ns] = *std::max_element(grad_ + beg_[i], grad_ + end_[i]);
        ++ns;
      }
    }
    if (ns == 0) return;
    double slope_ref = np_percentile(slopes_, ns, DGX_SLOPE_REF_PCT, scratch_);
    int min_delay = win(DGX_MIN_DELAY_S);
    int last = -10 * min_delay;
    for (int i = 0; i < ns && nb_ < kMaxBeats; ++i) {
      if (slopes_[i] < DGX_SLOPE_FRAC * slope_ref) continue;  // tall T wave, not a QRS
      int r = beg_[i] + argmax(y + beg_[i], end_[i] - beg_[i]);
      if (r - last > min_delay) {
        R_[nb_++] = r;
        last = r;
      }
    }
  }

  void delineate() {
    const int n = n_, nb = nb_;
    const double* y = y_;
    const int wq = win(DGX_Q_WIN_S), ws = win(DGX_S_WIN_S), wp0 = win(DGX_P_WIN_START_S), wp1 = win(DGX_P_WIN_END_S);
    const int wt0 = win(DGX_T_WIN_START_S), wt1 = win(DGX_T_WIN_END_S);
    for (int i = 0; i < nb; ++i) {
      int ri = R_[i];
      int rr_prev = i > 0 ? R_[i] - R_[i - 1] : 0;
      int rr_next = i < nb - 1 ? R_[i + 1] - R_[i] : 0;
      int rr_ref = rr_next > 0 ? rr_next : rr_prev;
      P_[i] = Q_[i] = S_[i] = T_[i] = -1;
      int a = ri - wq;
      if (a >= 0 && ri > a) Q_[i] = a + argmin(y + a, ri - a);
      int b = ri + ws;
      if (b < n) S_[i] = ri + 1 + argmin(y + ri + 1, b - ri);
      int p0 = ri - wp0, p1 = ri - wp1;
      if (rr_prev > 0) p0 = std::max(p0, R_[i - 1] + rr_prev / 2);
      if (p0 >= 0 && p1 > p0) P_[i] = p0 + argmax(y + p0, p1 - p0);
      int t0 = ri + wt0;
      int t1 = ri + (rr_ref > 0 ? std::min(wt1, (int)(0.65 * rr_ref)) : wt1);
      if (t1 < n && t1 > t0) T_[i] = t0 + argmax(y + t0, t1 - t0 + 1);
    }
  }

  // mean/SD of successive differences of valid fiducials, in seconds
  void successive(const int* idx, double& mean, double& sd) const {
    double v[kMaxBeats];
    int m = 0;
    for (int i = 0; i + 1 < nb_; ++i)
      if (idx[i] >= 0 && idx[i + 1] >= 0) v[m++] = (double)(idx[i + 1] - idx[i]) / kFs;
    mean_sd(v, m, mean, sd);
  }

  void amplitudes(const int* idx, double& mean, double& sd) const {
    double v[kMaxBeats];
    int m = 0;
    for (int i = 0; i < nb_; ++i)
      if (idx[i] >= 0) v[m++] = y_[idx[i]];
    mean_sd(v, m, mean, sd);
  }

  void compute_features(EcgFeatures& f) {
    double* v = f.value;
    for (int i = 0; i < kNumFeatures; ++i) v[i] = NAN;
    f.n_beats = nb_;
    f.rr_ratio = f.beat_corr = NAN;
    f.sqi_ok = false;
    if (nb_ < 2) return;
    const int nrr = nb_ - 1;
    double rr[kMaxBeats], hr[kMaxBeats], tmp[kMaxBeats], unused;
    for (int i = 0; i < nrr; ++i) {
      rr[i] = (double)(R_[i + 1] - R_[i]) / kFs;
      hr[i] = 60.0 / rr[i];
    }
    mean_sd(rr, nrr, v[F_MEAN_RR_S], v[F_SD_RR_S]);
    for (int i = 0; i < nb_; ++i) tmp[i] = y_[R_[i]];
    mean_sd(tmp, nb_, v[F_MEAN_R_UV], v[F_SD_R_UV]);
    successive(P_, v[F_MEAN_PP_S], v[F_SD_PP_S]);
    successive(T_, v[F_MEAN_TT_S], v[F_SD_TT_S]);
    successive(Q_, v[F_MEAN_QQ_S], v[F_SD_QQ_S]);
    successive(S_, v[F_MEAN_SS_S], v[F_SD_SS_S]);
    amplitudes(P_, v[F_MEAN_P_UV], v[F_SD_P_UV]);
    amplitudes(T_, v[F_MEAN_T_UV], v[F_SD_T_UV]);
    amplitudes(Q_, v[F_MEAN_Q_UV], v[F_SD_Q_UV]);
    amplitudes(S_, v[F_MEAN_S_UV], v[F_SD_S_UV]);
    int m = 0;
    for (int i = 0; i < nb_; ++i)
      if (Q_[i] >= 0 && S_[i] >= 0) tmp[m++] = (double)(S_[i] - Q_[i]) / kFs * 1000;
    mean_sd(tmp, m, v[F_QRS_MEAN_MS], v[F_QRS_SD_MS]);
    if (nrr >= 2) {
      for (int i = 0; i + 1 < nrr; ++i) {
        double d = rr[i + 1] - rr[i];
        tmp[i] = d * d;
      }
      v[F_RMSSD_MS] = sqrt(np_mean(tmp, nrr - 1)) * 1000;
      double hm;
      mean_sd(hr, nrr, hm, v[F_HR_SD_BPM]);
    }
    v[F_HR_MEDIAN_BPM] = np_median(hr, nrr, tmp);
    v[F_HR_MIN_BPM] = *std::min_element(hr, hr + nrr);
    v[F_HR_MAX_BPM] = *std::max_element(hr, hr + nrr);
    v[F_HR_MEAN_BPM] = np_mean(hr, nrr);
    m = 0;
    for (int i = 0; i < nb_; ++i)
      if (P_[i] >= 0) tmp[m++] = (double)(R_[i] - P_[i]) / kFs * 1000;
    mean_sd(tmp, m, v[F_PR_PEAK_MS], unused);
    m = 0;
    for (int i = 0; i < nb_; ++i)
      if (Q_[i] >= 0 && T_[i] >= 0) tmp[m++] = (double)(T_[i] - Q_[i]) / kFs * 1000;
    mean_sd(tmp, m, v[F_QT_PEAK_MS], unused);
    v[F_QTC_PEAK_MS] = v[F_QT_PEAK_MS] / sqrt(v[F_MEAN_RR_S]);
    v[F_T_R_RATIO] = v[F_MEAN_R_UV] > 0 ? v[F_MEAN_T_UV] / v[F_MEAN_R_UV] : NAN;
    f.rr_ratio = *std::max_element(rr, rr + nrr) / *std::min_element(rr, rr + nrr);
    f.beat_corr = beat_correlation();
    double hr_mean = v[F_HR_MEAN_BPM];
    f.sqi_ok = nb_ >= DGX_SQI_MIN_BEATS && hr_mean >= DGX_SQI_HR_MIN && hr_mean <= DGX_SQI_HR_MAX &&
               f.rr_ratio <= DGX_SQI_MAX_RR_RATIO && v[F_MEAN_R_UV] > 0 && f.beat_corr >= DGX_SQI_MIN_BEAT_CORR;
  }

  // Mean Pearson correlation of each beat [R - 0.2 s, R + 0.4 s) with the average beat (>= 3 beats).
  double beat_correlation() {
    const int a = win(DGX_BEAT_WIN_PRE_S), b = win(DGX_BEAT_WIN_POST_S), L = a + b;
    int idx[kMaxBeats], k = 0;
    for (int i = 0; i < nb_; ++i)
      if (R_[i] - a >= 0 && R_[i] + b <= n_) idx[k++] = R_[i] - a;
    if (k < 3) return NAN;
    double* tpl = scratch_;      // L <= kMaxSegments
    double* xc = slopes_;        // centred beat
    double* tc = scratch_ + L;   // centred template
    double* prod = cs_;          // element-wise products
    for (int j = 0; j < L; ++j) tpl[j] = 0.0;
    for (int i = 0; i < k; ++i)
      for (int j = 0; j < L; ++j) tpl[j] += y_[idx[i] + j];
    for (int j = 0; j < L; ++j) tpl[j] /= k;
    double tmean = np_sum(tpl, L) / L;
    for (int j = 0; j < L; ++j) tc[j] = tpl[j] - tmean;
    for (int j = 0; j < L; ++j) prod[j] = tc[j] * tc[j];
    const double inv = 1.0 / (L - 1);
    const double sd_t = sqrt(np_sum(prod, L) * inv);
    double corr[kMaxBeats];
    for (int i = 0; i < k; ++i) {
      const double* bt = y_ + idx[i];
      double bmean = np_sum(bt, L) / L;
      for (int j = 0; j < L; ++j) xc[j] = bt[j] - bmean;
      for (int j = 0; j < L; ++j) prod[j] = xc[j] * xc[j];
      double sd_b = sqrt(np_sum(prod, L) * inv);
      for (int j = 0; j < L; ++j) prod[j] = xc[j] * tc[j];
      double c = np_sum(prod, L) * inv / sd_b / sd_t;   // np.corrcoef: cov / sd_x / sd_y, clipped
      corr[i] = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
    }
    return np_mean(corr, k);
  }

  void* block_ = nullptr;
  double *in_ = nullptr, *ext_ = nullptr, *grad_ = nullptr, *sm_ = nullptr, *av_ = nullptr, *cs_ = nullptr;
  double *slopes_ = nullptr, *scratch_ = nullptr;
  int *beg_ = nullptr, *end_ = nullptr;
  const double* y_ = nullptr;
  int n_ = 0, nb_ = 0;
  int R_[kMaxBeats], P_[kMaxBeats], Q_[kMaxBeats], S_[kMaxBeats], T_[kMaxBeats];
};

}  // namespace dgx
