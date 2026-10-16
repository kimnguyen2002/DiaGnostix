"""
DiaGnostix-Edge single-lead (lead I) ECG feature extractor -- Python reference implementation.

This module is the *specification* that the ESP32-S3 firmware must reproduce. Every step maps 1:1
onto plain C so that the features the model sees on the device are computed exactly as in training:

  1. 10-s buffer of lead I in microvolts (AD8232 OUT -> ESP32-S3 ADC1 at 500 Hz -> mV -> uV using the
     calibrated AD8232 gain; see simulate_esp32s3_adc for the acquisition model used in training).
  2. Zero-phase band-pass 0.5-40 Hz: 2nd-order Butterworth band-pass designed as second-order
     sections (biquads), run forward then backward (Direct-Form II transposed, zero initial state)
     over the buffer padded with 1 s of odd reflection at both ends.
  3. R-peak detection with the gradient/boxcar method of NeuroKit2 ("neurokit", Makowski 2021) --
     the same detector family used to build the published DiaCardia features -- simplified to use the
     arg-max inside each QRS segment instead of the most prominent local maximum, plus a slope-based
     T-wave rejection rule (Pan & Tompkins 1985 style).
  4. Fixed-window delineation around every R peak (Q, S, P, T fiducials as local extrema).
  5. 28 record-level features mirroring the DiaCardia lead-I list + 4 physiologically motivated extras.
  6. A signal-quality index (SQI: beat count, heart-rate range, RR consistency, beat-to-template
     correlation) so the device can refuse to classify unusable or noisy recordings.

All windows are expressed in seconds, so the same code runs at 125, 250 or 500 Hz.
The C++ port is firmware/DiaGnostixECG/ecg_features.h; firmware/test checks it against golden vectors.
"""
from fractions import Fraction

import numpy as np
from joblib import Parallel, delayed
from scipy import signal as sps

# ------------------------------------------------------------------ feature names
EDGE_CORE = [  # same order/semantics as feature_list_28_1-lead.csv (DiaCardia), SI units
    "mean_rr_s", "sd_rr_s", "mean_r_uv", "sd_r_uv", "mean_pp_s", "sd_pp_s", "mean_p_uv", "sd_p_uv",
    "mean_tt_s", "sd_tt_s", "mean_t_uv", "sd_t_uv", "mean_qq_s", "sd_qq_s", "mean_q_uv", "sd_q_uv",
    "mean_ss_s", "sd_ss_s", "mean_s_uv", "sd_s_uv", "qrs_mean_ms", "qrs_sd_ms",
    "rmssd_ms",  # true RMSSD; the published 'RMSSD_I' is actually mean((60/RR)^2) (see notebook)
    "hr_sd_bpm", "hr_median_bpm", "hr_min_bpm", "hr_max_bpm", "hr_mean_bpm",
]
EDGE_EXTRA = ["pr_peak_ms", "qt_peak_ms", "qtc_peak_ms", "t_r_ratio"]
EDGE_ALL = EDGE_CORE + EDGE_EXTRA
AMPLITUDE_FEATURES = [f for f in EDGE_ALL if f.endswith("_uv")]
GAIN_INVARIANT = [f for f in EDGE_ALL if f not in AMPLITUDE_FEATURES]   # timing/HRV + T/R ratio
SQI_FIELDS = ["n_beats", "rr_ratio", "beat_corr", "sqi_ok"]

# mapping to the published NeuroKit2 feature names (for the parity analysis)
EDGE_TO_NK2 = dict(zip(EDGE_CORE, [
    "mean_rr_interval_I", "sd_rr_interval_I", "mean_r_peak_I", "sd_r_peak_I", "mean_pp_interval_I",
    "sd_pp_interval_I", "mean_p_peak_I", "sd_p_peak_I", "mean_tt_interval_I", "sd_tt_interval_I",
    "mean_t_peak_I", "sd_t_peak_I", "mean_qq_interval_I", "sd_qq_interval_I", "mean_q_peak_I",
    "sd_q_peak_I", "mean_ss_interval_I", "sd_ss_interval_I", "mean_s_peak_I", "sd_s_peak_I",
    "qrs_mean_I", "qrs_std_I", "RMSSD_I", "R_HR_STD_I", "R_HR_median_I", "R_HR_min_I", "R_HR_max_I",
    "mean_heartrate_r_I"]))

# ------------------------------------------------------------------ algorithm constants (seconds)
BAND_HZ = (0.5, 40.0)
PAD_S = 1.0
SMOOTH_S, AVG_S, THR_WEIGHT, MIN_LEN_WEIGHT, MIN_DELAY_S = 0.10, 0.75, 1.5, 0.4, 0.30
SLOPE_FRAC, SLOPE_REF_PCT = 0.5, 90  # percentile = numpy 'linear' interpolation on sorted slopes
Q_WIN_S, S_WIN_S = 0.06, 0.08
P_WIN_S = (0.30, 0.08)          # P peak searched in [R-0.30 s, R-0.08 s)
T_WIN_S = (0.10, 0.45)          # T peak searched in [R+0.10 s, R+min(0.45 s, 0.65*RR)]
SQI_MIN_BEATS, SQI_HR_RANGE, SQI_MAX_RR_RATIO = 5, (30.0, 200.0), 2.5
BEAT_WIN_S, SQI_MIN_BEAT_CORR = (0.20, 0.40), 0.90   # beat-to-template correlation (noise gate)


def design_bandpass(fs):
    """Biquad coefficients (n_sections x 6: b0 b1 b2 a0 a1 a2) -- exported verbatim to the C header."""
    return sps.butter(2, BAND_HZ, btype="bandpass", fs=fs, output="sos")


def zero_phase_filter(x, sos, fs):
    pad = int(PAD_S * fs)
    ext = np.concatenate([2 * x[0] - x[pad:0:-1], x, 2 * x[-1] - x[-2:-pad - 2:-1]])
    y = sps.sosfilt(sos, ext)               # DF-II transposed, zero initial state
    y = sps.sosfilt(sos, y[::-1])[::-1]
    return y[pad:-pad]


def moving_average(x, k):
    """Centred boxcar of length k with edge replication (cumulative-sum implementation)."""
    if k <= 1:
        return x.astype(float).copy()
    h = k // 2
    xp = np.concatenate([np.full(h, x[0]), x, np.full(k - 1 - h, x[-1])])
    c = np.concatenate([[0.0], np.cumsum(xp)])
    return (c[k:] - c[:-k]) / k


def detect_r_peaks(y, fs):
    absgrad = np.abs(np.gradient(y))
    smooth = moving_average(absgrad, int(round(SMOOTH_S * fs)))
    avg = moving_average(smooth, int(round(AVG_S * fs)))
    qrs = smooth > THR_WEIGHT * avg
    beg = np.flatnonzero(~qrs[:-1] & qrs[1:])
    end = np.flatnonzero(qrs[:-1] & ~qrs[1:])
    if beg.size == 0:
        return np.array([], dtype=int)
    end = end[end > beg[0]]
    n = min(beg.size, end.size)
    if n == 0:
        return np.array([], dtype=int)
    beg, end = beg[:n], end[:n]
    min_len = MIN_LEN_WEIGHT * np.mean(end - beg)
    segs = [(b, e) for b, e in zip(beg, end) if e - b >= min_len]
    if not segs:
        return np.array([], dtype=int)
    # T-wave rejection (added to the NeuroKit2 logic): a QRS complex is the steepest deflection in the
    # beat, so candidate segments whose peak slope is < 50 % of the 90th-percentile slope are tall
    # T waves. Cuts gross heart-rate errors vs. the measured pulse rate from 2.2 % to 0.2 %.
    slopes = np.array([absgrad[b:e].max() for b, e in segs])
    slope_ref = np.percentile(slopes, SLOPE_REF_PCT)
    min_delay = int(round(MIN_DELAY_S * fs))
    peaks, last = [], -10 * min_delay
    for (b, e), slope in zip(segs, slopes):
        if slope < SLOPE_FRAC * slope_ref:
            continue
        r = b + int(np.argmax(y[b:e]))
        if r - last > min_delay:
            peaks.append(r)
            last = r
    return np.asarray(peaks, dtype=int)


def delineate(y, r, fs):
    """Return fiducial index arrays (P, Q, R, S, T); -1 where a fiducial is not measurable."""
    n, nb = len(y), len(r)
    w = lambda s: int(round(s * fs))
    P, Q, S, T = (np.full(nb, -1, dtype=int) for _ in range(4))
    for i, ri in enumerate(r):
        rr_prev = r[i] - r[i - 1] if i > 0 else 0
        rr_next = r[i + 1] - r[i] if i < nb - 1 else 0
        rr_ref = rr_next if rr_next > 0 else rr_prev
        a = ri - w(Q_WIN_S)
        if a >= 0 and ri > a:
            Q[i] = a + int(np.argmin(y[a:ri]))
        b = ri + w(S_WIN_S)
        if b < n:
            S[i] = ri + 1 + int(np.argmin(y[ri + 1:b + 1]))
        p0, p1 = ri - w(P_WIN_S[0]), ri - w(P_WIN_S[1])
        if rr_prev > 0:
            p0 = max(p0, r[i - 1] + rr_prev // 2)
        if p0 >= 0 and p1 > p0:
            P[i] = p0 + int(np.argmax(y[p0:p1]))
        t0 = ri + w(T_WIN_S[0])
        t1 = ri + (min(w(T_WIN_S[1]), int(0.65 * rr_ref)) if rr_ref > 0 else w(T_WIN_S[1]))
        if t1 < n and t1 > t0:
            T[i] = t0 + int(np.argmax(y[t0:t1 + 1]))
    return P, Q, np.asarray(r, dtype=int), S, T


def _mean_sd(a):
    a = np.asarray(a, dtype=float)
    if a.size == 0:
        return np.nan, np.nan
    return float(a.mean()), (float(a.std()) if a.size >= 2 else np.nan)


def _successive(idx, fs):
    ok = (idx[:-1] >= 0) & (idx[1:] >= 0)
    return (idx[1:] - idx[:-1])[ok] / fs


def compute_features(y, fid, fs):
    P, Q, R, S, T = fid
    f = dict.fromkeys(EDGE_ALL, np.nan)
    f.update(n_beats=len(R), rr_ratio=np.nan, beat_corr=np.nan, sqi_ok=False)
    if len(R) < 2:
        return f
    rr = np.diff(R) / fs
    hr = 60.0 / rr
    f["mean_rr_s"], f["sd_rr_s"] = _mean_sd(rr)
    f["mean_r_uv"], f["sd_r_uv"] = _mean_sd(y[R])
    for key, idx in (("pp", P), ("tt", T), ("qq", Q), ("ss", S)):
        f[f"mean_{key}_s"], f[f"sd_{key}_s"] = _mean_sd(_successive(idx, fs))
    for key, idx in (("p", P), ("t", T), ("q", Q), ("s", S)):
        f[f"mean_{key}_uv"], f[f"sd_{key}_uv"] = _mean_sd(y[idx[idx >= 0]])
    both = (Q >= 0) & (S >= 0)
    f["qrs_mean_ms"], f["qrs_sd_ms"] = _mean_sd((S[both] - Q[both]) / fs * 1000)
    f["rmssd_ms"] = float(np.sqrt(np.mean(np.diff(rr) ** 2)) * 1000) if rr.size >= 2 else np.nan
    f["hr_sd_bpm"] = float(hr.std()) if hr.size >= 2 else np.nan
    f["hr_median_bpm"], f["hr_min_bpm"] = float(np.median(hr)), float(hr.min())
    f["hr_max_bpm"], f["hr_mean_bpm"] = float(hr.max()), float(hr.mean())
    okp = P >= 0
    f["pr_peak_ms"] = _mean_sd((R[okp] - P[okp]) / fs * 1000)[0]
    okt = (Q >= 0) & (T >= 0)
    f["qt_peak_ms"] = _mean_sd((T[okt] - Q[okt]) / fs * 1000)[0]
    f["qtc_peak_ms"] = f["qt_peak_ms"] / np.sqrt(f["mean_rr_s"])
    f["t_r_ratio"] = f["mean_t_uv"] / f["mean_r_uv"] if f["mean_r_uv"] > 0 else np.nan
    f["rr_ratio"] = float(rr.max() / rr.min())
    # noise gate: mean Pearson correlation of each beat [R-0.2 s, R+0.4 s] with the average beat
    a, b = int(round(BEAT_WIN_S[0] * fs)), int(round(BEAT_WIN_S[1] * fs))
    beats = np.array([y[r - a:r + b] for r in R if r - a >= 0 and r + b <= len(y)])
    if len(beats) >= 3:
        tpl = beats.mean(axis=0)
        f["beat_corr"] = float(np.mean([np.corrcoef(bt, tpl)[0, 1] for bt in beats]))
    f["sqi_ok"] = bool(len(R) >= SQI_MIN_BEATS and SQI_HR_RANGE[0] <= f["hr_mean_bpm"] <= SQI_HR_RANGE[1]
                       and f["rr_ratio"] <= SQI_MAX_RR_RATIO and f["mean_r_uv"] > 0
                       and f["beat_corr"] >= SQI_MIN_BEAT_CORR)
    return f


# ------------------------------------------------------------------ device simulation (training only)
def simulate_ad8232(x_uv, fs, hp_hz=0.5, lp_hz=40.0):
    """Causal analogue front-end of the common AD8232 breakout: 2-pole HPF 0.5 Hz + 2-pole LPF 40 Hz."""
    sos = np.vstack([sps.butter(2, hp_hz, "highpass", fs=fs, output="sos"),
                     sps.butter(2, lp_hz, "lowpass", fs=fs, output="sos")])
    y, _ = sps.sosfilt(sos, x_uv, zi=sps.sosfilt_zi(sos) * x_uv[0])
    return y


def resample(x, fs_in, fs_out):
    if fs_in == fs_out:
        return x
    fr = Fraction(fs_out / fs_in).limit_denominator(1000)
    return sps.resample_poly(x, fr.numerator, fr.denominator)


def add_noise(x_uv, fs, snr_db, rng, mains_hz=60.0):
    """Wearable-type noise. SNR is defined *inside the 0.5-40 Hz analysis band* (what reaches the model).
    In-band mix: 50 % EMG-like broadband noise, 50 % electrode-motion artefact (noise low-passed at 8 Hz);
    out-of-band baseline wander (0.05-0.35 Hz) and 60 Hz mains are added on top at the same in-band level."""
    n = len(x_uv)
    t = np.arange(n) / fs
    band = design_bandpass(fs)
    p_sig = np.var(sps.sosfiltfilt(band, x_uv))
    emg = rng.standard_normal(n)
    motion = sps.sosfiltfilt(sps.butter(2, 8.0, "lowpass", fs=fs, output="sos"), rng.standard_normal(n))
    inband = sum(c / np.std(sps.sosfiltfilt(band, c)) * np.sqrt(w) for c, w in ((emg, 0.5), (motion, 0.5)))
    wander = (np.sin(2 * np.pi * rng.uniform(0.15, 0.35) * t + rng.uniform(0, 2 * np.pi))
              + 0.5 * np.sin(2 * np.pi * rng.uniform(0.05, 0.10) * t + rng.uniform(0, 2 * np.pi)))
    mains = np.sin(2 * np.pi * mains_hz * t + rng.uniform(0, 2 * np.pi))
    scale = np.sqrt(p_sig / 10 ** (snr_db / 10))
    out_of_band = (wander / wander.std() + mains / mains.std()) * scale
    return x_uv + inband * scale + out_of_band


# ------------------------------------------------------------------ end-to-end
def device_features(x_uv, fs, return_debug=False):
    """What the firmware does: 10-s ADC stream in microvolts -> features. (Golden-vector entry point.)"""
    x_uv = np.asarray(x_uv, dtype=float)
    y = zero_phase_filter(x_uv - np.median(x_uv), design_bandpass(fs), fs)
    fid = delineate(y, detect_r_peaks(y, fs), fs)
    feats = compute_features(y, fid, fs)
    if return_debug:
        return feats, {"x": x_uv, "y": y, "fs": fs, "fiducials": fid}
    return feats


def simulate_esp32s3_adc(x_uv, rng, frontend_gain=1100.0, assumed_gain=1100.0, ref_mv=1650.0,
                         full_scale_mv=3100.0, lsb_mv=1.0, noise_mv=3.0):
    """ESP32-S3 ADC1 reading the AD8232 output, then the firmware conversion back to microvolts.

    frontend_gain: the true gain of this AD8232 board; assumed_gain: the gain the firmware divides by
    (their ratio is the amplitude-calibration error). analogReadMilliVolts() returns integer mV, so the
    resolution referred to the electrodes is lsb_mv / gain ~ 0.9 uV. ADC + supply noise is white Gaussian."""
    v = ref_mv + np.asarray(x_uv, dtype=float) * frontend_gain / 1000.0
    if noise_mv > 0:
        v = v + rng.normal(0.0, noise_mv, len(v))
    v = np.clip(v, 0.0, full_scale_mv)
    v = np.round(v / lsb_mv) * lsb_mv
    return (v - ref_mv) * 1000.0 / assumed_gain


def process_record(x_lsb, fs_in=500, fs_dev=500, frontend=True, adc="esp32s3", gain=1.0, snr_db=None,
                   clock_error=0.0, adc_noise_mv=3.0, seed=0, uv_per_lsb=5.0, return_debug=False):
    """Clinical 500-Hz record -> simulated device stream -> features.

    frontend: AD8232 analogue filters; adc: "esp32s3" (this device: 12-bit SAR ADC read in integer mV at
    500 Hz) or None (ideal); gain: true/assumed AD8232 gain ratio (amplitude-calibration error);
    snr_db: wearable noise added at the electrodes; clock_error: sampling-clock error while the firmware
    still assumes fs_dev (the ESP32-S3 timer runs from a crystal, so 0 is realistic; kept for comparison
    with external ADCs such as the ADS1115)."""
    rng = np.random.default_rng(seed)
    x = np.asarray(x_lsb, dtype=float) * uv_per_lsb
    if snr_db is not None:
        x = add_noise(x, fs_in, snr_db, rng)
    if frontend:
        x = simulate_ad8232(x, fs_in)
    x = resample(x, fs_in, fs_dev * (1.0 + clock_error))
    if adc == "esp32s3":
        x = simulate_esp32s3_adc(x, rng, frontend_gain=1100.0 * gain, assumed_gain=1100.0, noise_mv=adc_noise_mv)
    elif gain != 1.0:
        x = x * gain
    return device_features(x, fs_dev, return_debug)


def _chunk(X, ids, kw):
    out = []
    for x, i in zip(X, ids):
        out.append(process_record(x, seed=int(i), **kw))
    return out


def extract_batch(X_lsb, n_jobs=-1, chunk=256, **kw):
    """Run process_record over a (n_records, n_samples) array; returns a list of feature dicts."""
    idx = np.arange(len(X_lsb))
    parts = Parallel(n_jobs=n_jobs)(delayed(_chunk)(X_lsb[s:s + chunk], idx[s:s + chunk], kw)
                                    for s in range(0, len(X_lsb), chunk))
    return [f for p in parts for f in p]
