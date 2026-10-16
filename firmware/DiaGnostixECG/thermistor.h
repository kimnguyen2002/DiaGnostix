#pragma once

#include <cmath>

namespace dsp {

// 10 kOhm NTC in a voltage divider with a fixed series resistor.
//
//   ntc_on_low_side = true :  VCC -- R_series --+-- NTC -- GND   (ADC at '+')
//   ntc_on_low_side = false:  VCC -- NTC --+-- R_series -- GND
struct ThermistorConfig {
  float series_ohm = 10000.0f;
  float nominal_ohm = 10000.0f;  // NTC resistance at nominal_c
  float nominal_c = 25.0f;
  float beta = 3950.0f;
  float supply_mv = 3300.0f;
  bool ntc_on_low_side = true;
};

inline float thermistorResistance(float adc_mv, const ThermistorConfig& c) {
  if (adc_mv <= 1.0f || adc_mv >= c.supply_mv - 1.0f) return NAN;
  return c.ntc_on_low_side ? c.series_ohm * adc_mv / (c.supply_mv - adc_mv)
                           : c.series_ohm * (c.supply_mv - adc_mv) / adc_mv;
}

// Beta-parameter form of the Steinhart-Hart equation.
inline float thermistorCelsius(float resistance_ohm, const ThermistorConfig& c) {
  if (!(resistance_ohm > 0.0f)) return NAN;
  const float t0 = c.nominal_c + 273.15f;
  const float inv_t = 1.0f / t0 + std::log(resistance_ohm / c.nominal_ohm) / c.beta;
  return 1.0f / inv_t - 273.15f;
}

inline float thermistorCelsiusFromMv(float adc_mv, const ThermistorConfig& c) {
  return thermistorCelsius(thermistorResistance(adc_mv, c), c);
}

}  // namespace dsp
