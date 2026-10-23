# Host unit tests for the DiaGnostix-ECG sketch.
#
#   make -C firmware/test          build and run all tests
#   make -C firmware/test clean
#
# test_dsp : streaming QRS, PPG, pulse arrival time, BP, thermistor (from DiaGnostix)
# test_smoothing : vital-sign smoother (jitter, outliers, step response); --csv writes the streams
# test_ecg : ECG feature extractor and the generated risk models vs the Python golden vectors
#            written by ml/notebooks/01_ecg_prediabetes_training_and_export.ipynb (Section 15)

CXX ?= c++
# -ffp-contract=off: no fused multiply-add, so the double arithmetic matches numpy operation by operation
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -ffp-contract=off
SKETCH ?= ../DiaGnostixECG
MODELS ?= $(SKETCH)
GOLDEN ?= golden
DSP_HEADERS := $(addprefix $(SKETCH)/,biquad.h stats.h qrs_detector.h ppg_processor.h pat_pairer.h bp_estimator.h thermistor.h)
ECG_HEADERS := $(addprefix $(SKETCH)/,ecg_features.h risk_model.h ecg_risk.h) \
               $(addprefix $(MODELS)/,dgx_models.h dgx_dsp_spec.h dgx_selftest_vector.h)

.PHONY: test clean

test: test_dsp test_smoothing test_ecg
	./test_dsp
	./test_smoothing
	./test_ecg $(GOLDEN)

test_dsp: test_dsp.cpp test_framework.h $(DSP_HEADERS)
	$(CXX) $(CXXFLAGS) -I$(SKETCH) test_dsp.cpp -o $@

test_smoothing: test_smoothing.cpp test_framework.h $(SKETCH)/smoothing.h
	$(CXX) $(CXXFLAGS) -I$(SKETCH) test_smoothing.cpp -o $@

test_ecg: test_ecg.cpp test_framework.h $(ECG_HEADERS)
	$(CXX) $(CXXFLAGS) -I$(SKETCH) -I$(MODELS) test_ecg.cpp -o $@

clean:
	rm -f test_dsp test_smoothing test_ecg
