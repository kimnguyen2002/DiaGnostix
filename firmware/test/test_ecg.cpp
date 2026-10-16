// Host tests of the ECG risk pipeline of the sketch against the Python reference. Run with:
//   make -C firmware/test
//
// golden/ is written by the notebook (Section 15): device-domain ECG streams in uV, the features the
// Python extractor computed from them, the profiles and the three model scores. The C++ extractor and
// the generated model headers must reproduce them before the firmware is trusted on a person.

#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dgx_selftest_vector.h"
#include "ecg_risk.h"

using namespace dgx;

void setUp() {}
void tearDown() {}

namespace {

std::string g_dir = "golden";

constexpr double ECG_GAIN = 1100.0;   // gain and REF of the training simulation (ml/dgx/config.py)
constexpr double ECG_REF_MV = 1650.0;
constexpr int ECG_CLIP_MV = 3100;

struct Csv {
  std::vector<std::string> header;  // without the id column
  std::vector<std::string> ids;
  std::vector<std::vector<double>> rows;
  int col(const char* name) const {
    for (size_t i = 0; i < header.size(); ++i)
      if (header[i] == name) return static_cast<int>(i);
    testfw::fail(__LINE__, std::string("missing column ") + name);
  }
};

Csv readCsv(const std::string& file, bool has_header) {
  std::ifstream in(g_dir + "/" + file);
  if (!in) testfw::fail(__LINE__, "cannot open " + g_dir + "/" + file);
  Csv t;
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::stringstream ss(line);
    std::string cell, id;
    std::getline(ss, id, ',');
    std::vector<std::string> cells;
    while (std::getline(ss, cell, ',')) cells.push_back(cell);
    if (first && has_header) {
      t.header = cells;
    } else {
      std::vector<double> v;
      for (const std::string& c : cells) v.push_back(c == "nan" || c.empty() ? NAN : std::strtod(c.c_str(), nullptr));
      t.ids.push_back(id);
      t.rows.push_back(v);
    }
    first = false;
  }
  return t;
}

struct Golden {
  Csv signals, features, profiles, scores;
  std::vector<std::vector<int16_t>> mv;  // integer mV at GPIO4, as analogReadMilliVolts() returns them
};

const Golden& golden() {
  static Golden g;
  static bool loaded = false;
  if (loaded) return g;
  g.signals = readCsv("signals_uv.csv", false);
  g.features = readCsv("features.csv", true);
  g.profiles = readCsv("profiles.csv", true);
  g.scores = readCsv("scores.csv", true);
  for (const auto& row : g.signals.rows) {
    if (row.size() != static_cast<size_t>(kWindowSamples)) testfw::fail(__LINE__, "golden signal is not 10 s at 500 Hz");
    std::vector<int16_t> m(row.size());
    for (size_t i = 0; i < row.size(); ++i)
      m[i] = static_cast<int16_t>(std::nearbyint(row[i] * ECG_GAIN / 1000.0 + ECG_REF_MV));
    g.mv.push_back(m);
  }
  loaded = true;
  return g;
}

// Same conversion as EcgRiskEngine::evaluate.
void loadWindow(EcgFeatureExtractor& fx, const int16_t* mv) {
  double* x = fx.input();
  for (int i = 0; i < kWindowSamples; ++i) x[i] = ((double)mv[i] - ECG_REF_MV) * 1000.0 / ECG_GAIN;
}

bool close(double expected, double actual, double rel) {
  if (std::isnan(expected)) return std::isnan(actual);
  return std::fabs(actual - expected) <= rel * std::fmax(1.0, std::fabs(expected));
}

NonEcgInputs profileOf(const Golden& g, size_t r) {
  const Csv& p = g.profiles;
  NonEcgInputs in;
  in.age = (float)p.rows[r][p.col("age")];
  in.male_sex = (float)p.rows[r][p.col("male_sex")];
  in.bmi = (float)p.rows[r][p.col("BMI")];
  in.sbp = (float)p.rows[r][p.col("sBP")];
  in.dbp = (float)p.rows[r][p.col("dBP")];
  in.smoking = (float)p.rows[r][p.col("smoking")];
  in.drinking = (float)p.rows[r][p.col("drinking")];
  return in;
}

size_t firstRecord(bool sqi_ok) {
  const Golden& g = golden();
  const int c = g.features.col("sqi_ok");
  for (size_t r = 0; r < g.features.rows.size(); ++r)
    if ((g.features.rows[r][c] != 0) == sqi_ok) return r;
  testfw::fail(__LINE__, "no suitable golden record");
}

}  // namespace

// Every feature of every golden record, computed from integer mV exactly as on the device.
void test_extractor_matches_python() {
  const Golden& g = golden();
  EcgFeatureExtractor fx;
  TEST_ASSERT_TRUE(fx.begin());
  double worst = 0.0;
  for (size_t r = 0; r < g.mv.size(); ++r) {
    TEST_ASSERT_TRUE(g.signals.ids[r] == g.features.ids[r]);
    loadWindow(fx, g.mv[r].data());
    EcgFeatures f;
    TEST_ASSERT_TRUE(fx.compute(f));
    const auto& e = g.features.rows[r];
    for (int k = 0; k < kNumFeatures; ++k) {
      const double ev = e[g.features.col(kFeatureNames[k])];
      if (!close(ev, f.value[k], 1e-9)) {
        char msg[200];
        std::snprintf(msg, sizeof msg, "record %s, %s: expected %.17g, was %.17g", g.features.ids[r].c_str(),
                      kFeatureNames[k], ev, f.value[k]);
        testfw::fail(__LINE__, msg);
      }
      if (!std::isnan(ev)) worst = std::fmax(worst, std::fabs(f.value[k] - ev) / std::fmax(1.0, std::fabs(ev)));
    }
    TEST_ASSERT_EQUAL_INT(e[g.features.col("n_beats")], f.n_beats);
    TEST_ASSERT_TRUE(close(e[g.features.col("rr_ratio")], f.rr_ratio, 1e-12));
    TEST_ASSERT_TRUE(close(e[g.features.col("beat_corr")], f.beat_corr, 1e-9));  // numpy uses BLAS here
    TEST_ASSERT_EQUAL_INT(e[g.features.col("sqi_ok")], f.sqi_ok ? 1 : 0);
  }
  std::printf("  %zu records, worst relative feature difference %.2e\n", g.mv.size(), worst);
}

// The three generated models, fed through the firmware's name-based input mapping.
void test_models_match_python_scores() {
  const Golden& g = golden();
  EcgFeatureExtractor fx;
  TEST_ASSERT_TRUE(fx.begin());
  const ModelRole roles[] = {ModelRole::kPrimary, ModelRole::kFallback, ModelRole::kEcgOnly};
  double worst = 0.0;
  for (size_t r = 0; r < g.mv.size(); ++r) {
    loadWindow(fx, g.mv[r].data());
    EcgFeatures f;
    TEST_ASSERT_TRUE(fx.compute(f));
    const NonEcgInputs in = profileOf(g, r);
    for (ModelRole role : roles) {
      const ModelSpec& m = modelSpec(role);
      float x[kMaxModelInputs];
      TEST_ASSERT_TRUE(fillInputs(m, f, in, x));
      const double s = m.score(x);
      const double e = g.scores.rows[r][g.scores.col(m.name)];
      TEST_ASSERT_FLOAT_WITHIN(1e-7, e, s);
      TEST_ASSERT_TRUE(m.tier(s) == m.tier(e));
      worst = std::fmax(worst, std::fabs(s - e));
    }
  }
  std::printf("  worst |score difference| %.2e over %zu records x 3 models\n", worst, g.mv.size());
}

// Three windows -> median score; lead-off windows are dropped; fewer than two good windows -> no result.
void test_session_median_and_lead_off() {
  const Golden& g = golden();
  const size_t r = firstRecord(true);
  std::vector<int16_t> buf(3 * kWindowSamples);
  for (int w = 0; w < 3; ++w) std::copy(g.mv[r].begin(), g.mv[r].end(), buf.begin() + w * kWindowSamples);
  EcgRiskEngine engine;
  TEST_ASSERT_TRUE(engine.begin());
  static RiskEstimate est;
  const NonEcgInputs none;
  engine.evaluate(buf.data(), 3, ECG_GAIN, ECG_REF_MV, false, ECG_CLIP_MV, ModelRole::kEcgOnly, none, 2, est);
  TEST_ASSERT_EQUAL_INT(3, est.n_good);
  const double expected = g.scores.rows[r][g.scores.col("ecg_only")];
  TEST_ASSERT_FLOAT_WITHIN(1e-7, expected, est.score);
  TEST_ASSERT_TRUE(est.tier == modelSpec(ModelRole::kEcgOnly).tier(expected));
  TEST_ASSERT_FLOAT_WITHIN(1e-12, modelSpec(ModelRole::kEcgOnly).calibrate(est.score), est.risk);

  buf[kWindowSamples + 1234] = kLeadOffSample;  // window 2: leads off for one sample
  engine.evaluate(buf.data(), 3, ECG_GAIN, ECG_REF_MV, false, ECG_CLIP_MV, ModelRole::kEcgOnly, none, 2, est);
  TEST_ASSERT_TRUE(est.window[1].status == WindowStatus::kLeadsOff);
  TEST_ASSERT_EQUAL_INT(2, est.n_good);
  TEST_ASSERT_TRUE(est.tier != RiskTier::kNoResult);

  buf[2 * kWindowSamples] = kLeadOffSample;     // window 3 as well: one good window is not enough
  engine.evaluate(buf.data(), 3, ECG_GAIN, ECG_REF_MV, false, ECG_CLIP_MV, ModelRole::kEcgOnly, none, 2, est);
  TEST_ASSERT_EQUAL_INT(1, est.n_good);
  TEST_ASSERT_TRUE(est.tier == RiskTier::kNoResult);
  TEST_ASSERT_TRUE(std::isnan(est.score));
}

// A recording that fails the signal-quality index never produces a risk.
void test_low_quality_gives_no_result() {
  const Golden& g = golden();
  const size_t r = firstRecord(false);
  std::vector<int16_t> buf(2 * kWindowSamples);
  for (int w = 0; w < 2; ++w) std::copy(g.mv[r].begin(), g.mv[r].end(), buf.begin() + w * kWindowSamples);
  EcgRiskEngine engine;
  TEST_ASSERT_TRUE(engine.begin());
  static RiskEstimate est;
  engine.evaluate(buf.data(), 2, ECG_GAIN, ECG_REF_MV, false, ECG_CLIP_MV, ModelRole::kEcgOnly, NonEcgInputs(), 2, est);
  TEST_ASSERT_TRUE(est.window[0].status == WindowStatus::kLowQuality);
  TEST_ASSERT_TRUE(est.tier == RiskTier::kNoResult);
}

// Inverting the leads (RA/LA swapped) is undone by ECG_INVERT.
void test_inverted_leads() {
  const Golden& g = golden();
  const size_t r = firstRecord(true);
  std::vector<int16_t> buf(2 * kWindowSamples);
  for (int w = 0; w < 2; ++w)
    for (int i = 0; i < kWindowSamples; ++i)
      buf[w * kWindowSamples + i] = static_cast<int16_t>(2 * (int)ECG_REF_MV - g.mv[r][i]);  // mirrored around REF
  EcgRiskEngine engine;
  TEST_ASSERT_TRUE(engine.begin());
  static RiskEstimate est;
  engine.evaluate(buf.data(), 2, ECG_GAIN, ECG_REF_MV, true, ECG_CLIP_MV, ModelRole::kEcgOnly, NonEcgInputs(), 2, est);
  TEST_ASSERT_EQUAL_INT(2, est.n_good);
  TEST_ASSERT_FLOAT_WITHIN(1e-7, g.scores.rows[r][g.scores.col("ecg_only")], est.score);
}

void test_model_choice() {
  NonEcgInputs none, prof;
  prof.age = 52;
  prof.male_sex = 1;
  prof.bmi = 24.5f;
  const char* why = nullptr;
  const auto needs = [](ModelRole r, const NonEcgInputs& p) { return nonEcgInputsAvailable(modelSpec(r), p); };
  // primary with a calibrated gain whenever its non-ECG inputs are available
  if (needs(ModelRole::kPrimary, prof)) TEST_ASSERT_TRUE(chooseModel(true, prof) == ModelRole::kPrimary);
  // without a calibrated gain only the gain-invariant fallback may run
  if (needs(ModelRole::kFallback, prof)) TEST_ASSERT_TRUE(chooseModel(false, prof) == ModelRole::kFallback);
  // no profile: ECG-only when the gain is calibrated
  TEST_ASSERT_TRUE(needs(ModelRole::kEcgOnly, none));
  const ModelRole r = chooseModel(true, none);
  TEST_ASSERT_TRUE(r == ModelRole::kPrimary || r == ModelRole::kEcgOnly);
  const ModelRole r2 = chooseModel(false, none, &why);
  if (r2 == ModelRole::kNone) TEST_ASSERT_TRUE(why != nullptr);
  // the fallback never uses an amplitude feature
  const ModelSpec& fb = modelSpec(ModelRole::kFallback);
  for (int i = 0; i < fb.n_inputs; ++i) {
    const std::string n = fb.input_names[i];
    TEST_ASSERT_FALSE(n.size() > 3 && n.compare(n.size() - 3, 3, "_uv") == 0);
  }
}

void test_all_model_inputs_known() {
  const char* unknown = "";
  const bool ok = modelInputsKnown(&unknown);
  if (!ok) testfw::fail(__LINE__, std::string("unknown model input ") + unknown);
  for (ModelRole r : {ModelRole::kPrimary, ModelRole::kFallback, ModelRole::kEcgOnly}) {
    const ModelSpec& m = modelSpec(r);
    TEST_ASSERT_TRUE(m.elevatedThreshold() <= m.highThreshold());
    TEST_ASSERT_TRUE(m.tier(1.0) == RiskTier::kHigh);
    TEST_ASSERT_TRUE(m.tier(0.0) == RiskTier::kLow);
  }
}

// Gain calibration with a 1 mV peak-to-peak 10 Hz sine read through the ADC (integer mV + 3 mV noise).
void test_gain_from_sine() {
  const double gain = 1043.0, ref_pp = 1.0;
  std::vector<int16_t> mv(2500);
  unsigned s = 7;
  for (size_t i = 0; i < mv.size(); ++i) {
    s = s * 1103515245u + 12345u;
    const double noise = 3.0 * (((s >> 8) & 0xffff) / 65535.0 - 0.5) * 3.46;  // uniform, SD ~3 mV
    const double v = ECG_REF_MV + gain * (ref_pp / 2) * std::sin(2 * M_PI * 10.0 * i / 500.0) + noise;
    mv[i] = static_cast<int16_t>(std::nearbyint(v));
  }
  const double est = EcgRiskEngine::gainFromSine(mv.data(), (int)mv.size(), ref_pp);
  TEST_ASSERT_FLOAT_WITHIN(0.01 * gain, gain, est);
  mv[100] = kLeadOffSample;
  TEST_ASSERT_TRUE(std::isnan(EcgRiskEngine::gainFromSine(mv.data(), (int)mv.size(), ref_pp)));
}

// The vector compiled into the sketch for the boot self-test.
void test_boot_selftest_vector() {
  EcgFeatureExtractor fx;
  TEST_ASSERT_TRUE(fx.begin());
  double* x = fx.input();
  for (int i = 0; i < kWindowSamples; ++i)
    x[i] = ((double)DGX_SELFTEST_MV[i] - DGX_SELFTEST_REF_MV) * 1000.0 / DGX_SELFTEST_GAIN;
  EcgFeatures f;
  TEST_ASSERT_TRUE(fx.compute(f));
  TEST_ASSERT_EQUAL_INT(DGX_SELFTEST_N_BEATS, f.n_beats);
  for (int k = 0; k < kNumFeatures; ++k) TEST_ASSERT_TRUE(close(DGX_SELFTEST_FEATURES[k], f.value[k], 1e-9));
  float in[kMaxModelInputs];
  const ModelSpec& m = modelSpec(ModelRole::kEcgOnly);
  TEST_ASSERT_TRUE(fillInputs(m, f, NonEcgInputs(), in));
  TEST_ASSERT_FLOAT_WITHIN(1e-7, DGX_SELFTEST_ECG_SCORE, m.score(in));
}

int main(int argc, char** argv) {
  if (argc > 1) g_dir = argv[1];
  std::printf("golden vectors: %s | model %s (%s), primary inputs: %s\n", g_dir.c_str(), DGX_MODEL_VERSION,
              DGX_MODEL_FAMILY, DGX_PRIMARY_INPUT_SET);
  UNITY_BEGIN();
  RUN_TEST(test_extractor_matches_python);
  RUN_TEST(test_models_match_python_scores);
  RUN_TEST(test_session_median_and_lead_off);
  RUN_TEST(test_low_quality_gives_no_result);
  RUN_TEST(test_inverted_leads);
  RUN_TEST(test_model_choice);
  RUN_TEST(test_all_model_inputs_known);
  RUN_TEST(test_gain_from_sine);
  RUN_TEST(test_boot_selftest_vector);
  return UNITY_END();
}
