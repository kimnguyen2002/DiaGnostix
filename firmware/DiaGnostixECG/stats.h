#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dsp {

// Fixed-capacity ring buffer that overwrites the oldest element when full.
template <typename T, size_t N>
class RingBuffer {
 public:
  void push(const T& v) {
    data_[head_] = v;
    head_ = (head_ + 1) % N;
    if (size_ < N) ++size_;
  }

  // i = 0 is the oldest element, i = size() - 1 the newest.
  const T& operator[](size_t i) const { return data_[(head_ + N - size_ + i) % N]; }
  T& operator[](size_t i) { return data_[(head_ + N - size_ + i) % N]; }

  const T& newest() const { return (*this)[size_ - 1]; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == N; }
  static constexpr size_t capacity() { return N; }
  void clear() { head_ = size_ = 0; }

 private:
  T data_[N] = {};
  size_t head_ = 0;
  size_t size_ = 0;
};

// Median of the first n values of src; uses scratch (size >= n) as workspace.
inline float median(const float* src, size_t n, float* scratch) {
  if (n == 0) return NAN;
  std::copy(src, src + n, scratch);
  std::nth_element(scratch, scratch + n / 2, scratch + n);
  float hi = scratch[n / 2];
  if (n % 2) return hi;
  float lo = *std::max_element(scratch, scratch + n / 2);
  return 0.5f * (lo + hi);
}

template <size_t N>
float median(const RingBuffer<float, N>& rb) {
  float values[N];
  float scratch[N];
  for (size_t i = 0; i < rb.size(); ++i) values[i] = rb[i];
  return median(values, rb.size(), scratch);
}

// Standard deviation of successive N-N intervals and root mean square of
// successive differences (time-domain HRV), in the unit of the input.
inline void hrvTimeDomain(const float* rr, size_t n, float* sdnn, float* rmssd) {
  *sdnn = *rmssd = NAN;
  if (n < 3) return;
  double mean = 0;
  for (size_t i = 0; i < n; ++i) mean += rr[i];
  mean /= n;
  double ss = 0, sd2 = 0;
  for (size_t i = 0; i < n; ++i) ss += (rr[i] - mean) * (rr[i] - mean);
  for (size_t i = 1; i < n; ++i) sd2 += (rr[i] - rr[i - 1]) * (rr[i] - rr[i - 1]);
  *sdnn = static_cast<float>(std::sqrt(ss / (n - 1)));
  *rmssd = static_cast<float>(std::sqrt(sd2 / (n - 1)));
}

// Vertex offset (in samples, within [-0.5, 0.5]) of the parabola through
// three equally spaced points with the middle one being the maximum.
inline float parabolicOffset(float ym1, float y0, float yp1) {
  const float denom = ym1 - 2.0f * y0 + yp1;
  if (std::fabs(denom) < 1e-12f) return 0.0f;
  const float d = 0.5f * (ym1 - yp1) / denom;
  return std::max(-0.5f, std::min(0.5f, d));
}

}  // namespace dsp
