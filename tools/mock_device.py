#!/usr/bin/env python3
"""Hardware-free simulator of a DiaGnostix-ECG device (firmware 3.0 protocol).

Serves the dashboard (firmware/web/index.html) at ``/``, the WebSocket protocol of docs/protocol.md at ``/ws``,
``GET /api/state`` and the captive-portal probe URLs, so the dashboard and the Flutter app can be developed
without an ESP32-S3.

Like the firmware, the simulator keeps user accounts (PBKDF2-HMAC-SHA256 password hashes, 10 000 iterations)
and per-user data (profile, blood-pressure calibration, results) in a directory (``--data``); every command
except register / login / auth needs a signed-in client, and live signals go only to the owner of a running
measurement.

The ECG is a real lead-I recording from the golden test vectors (firmware/test/golden/), replayed as the
integer millivolts the ESP32-S3 ADC would read. Each 10-s window is analysed as soon as it has been recorded
(the firmware's analysis task does the same on core 0) with the same Python pipeline and exported models as
the notebook (ml/dgx/edge_features.py, ml/outputs/models/); the result is finalised right after the recording.

    conda activate diagnostix
    python tools/mock_device.py                      # http://localhost:8080
    python tools/mock_device.py --risk high          # replay a recording the primary model rates high
    python tools/mock_device.py --board-gain 1043    # simulate an AD8232 board whose true gain is 1043
    python tools/mock_device.py --no-finger          # no finger on the MAX30102
    python tools/mock_device.py --data /tmp/dgx      # where accounts are stored (default: system temp dir)
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import logging
import re
import secrets
import sys
import tempfile
import time
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from aiohttp import WSMsgType, web

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ml"))
from dgx import edge_features as ef  # noqa: E402

GOLDEN = ROOT / "firmware" / "test" / "golden"
MODELS = ROOT / "ml" / "outputs" / "models"
INDEX_HTML = ROOT / "firmware" / "web" / "index.html"

DEVICE, FIRMWARE = "DiaGnostix-ECG", "3.0.0-mock"
FS, ECG_STREAM_FS, PPG_STREAM_FS = 500, 250, 50
REF_MV, NOMINAL_GAIN, ADC_MAX_MV, ADC_NOISE_MV = 1650.0, 1100.0, 3100, 3.0
WARMUP_S, WINDOW_S, MIN_WINDOWS, MAX_WINDOWS, MIN_GOOD = 3, 10, 2, 6, 2
GAIN_CAL_S, BP_CAL_S = 6, 20
MAX_USERS, PBKDF2_ITERATIONS, PASSWORD_MIN, HISTORY_MAX = 20, 10000, 6, 300
SESSION_TTL_S = 12 * 3600
USERNAME_RE = re.compile(r"^[a-z0-9._-]{3,24}$")
CAPTIVE_PROBES = ["/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                  "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/fwlink"]
BORDERLINE_WARNING = ("Borderline: the 10-s windows fell on both sides of a risk-tier boundary (the median was used); "
                      "repeat the measurement to confirm")
ADVICE = None  # filled from feature_spec.json
log = logging.getLogger("mock")


def num(v, nd=1):
    return None if v is None or not np.isfinite(v) else round(float(v), nd)


def logit(p):
    p = np.clip(p, 1e-6, 1 - 1e-6)
    return np.log(p / (1 - p))


class Models:
    """The three exported models with their thresholds and calibration (ml/outputs/models/feature_spec.json)."""

    def __init__(self):
        self.spec = json.loads((MODELS / "feature_spec.json").read_text())
        self.m = {role: joblib.load(MODELS / f"model_{role}.joblib") for role in self.spec["models"]}

    def info(self, role):
        return self.spec["models"][role]

    def non_ecg(self, role):
        return [c for c in self.info(role)["inputs"] if c not in ef.EDGE_ALL]

    def calibrate(self, role, score):
        pl = self.info(role)["platt"]
        return float(1 / (1 + np.exp(-(pl["a"] * logit(score) + pl["b"]))))

    def tier(self, role, score):
        t = self.info(role)["tiers_on_score"]
        return "high" if score >= t["high"] else ("elevated" if score >= t["elevated"] else "low")

    def borderline(self, role, scores):
        """Firmware rule (ecg_risk.h): the good windows straddle a tier boundary."""
        t = self.info(role)["tiers_on_score"]
        lo, hi = min(scores), max(scores)
        return any(lo < thr <= hi for thr in (t["high"], t["elevated"]))

    def score(self, role, feats, non_ecg):
        row = {**feats, **non_ecg}
        x = np.array([[row.get(c, np.nan) for c in self.info(role)["inputs"]]], dtype=np.float32).astype(float)
        return float(self.m[role].predict_proba(x)[0, 1])

    def choose(self, gain_calibrated, non_ecg):
        """Firmware rule (risk_model.h): calibrated gain -> primary, then ECG-only; otherwise the fallback."""
        order = ["primary", "ecg_only"] if gain_calibrated else ["fallback"]
        for role in order:
            if all(np.isfinite(non_ecg.get(c, np.nan)) for c in self.non_ecg(role)):
                return role
        return None


class UserStore:
    """Accounts and per-user data, one directory per user (the firmware uses /u/<id>/ on its FAT partition)."""

    DEFAULT_CAL = {"calibrated": False, "sbp": 120, "dbp": 80, "pat_ms": 250.0}

    def __init__(self, root: Path):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.accounts = {}
        for d in root.iterdir():
            f = d / "acct.json"
            if f.exists():
                a = json.loads(f.read_text())
                self.accounts[a["username"]] = a

    @staticmethod
    def _hash(password, salt, iterations):
        return hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)

    def create(self, username, password):
        if not USERNAME_RE.match(username):
            return "Username: 3-24 characters, lowercase letters, digits, dot, dash or underscore"
        if len(password) < PASSWORD_MIN:
            return f"Password: at least {PASSWORD_MIN} characters"
        if username in self.accounts:
            return "This username is taken"
        if len(self.accounts) >= MAX_USERS:
            return f"The device is full ({MAX_USERS} accounts); delete an unused account first"
        salt = secrets.token_bytes(16)
        a = {"username": username, "salt": salt.hex(), "iterations": PBKDF2_ITERATIONS,
             "hash": self._hash(password, salt, PBKDF2_ITERATIONS).hex()}
        (self.root / username).mkdir(exist_ok=True)
        (self.root / username / "acct.json").write_text(json.dumps(a))
        self.accounts[username] = a
        return None

    def verify(self, username, password):
        a = self.accounts.get(username)
        if not a:
            self._hash(password, b"0" * 16, PBKDF2_ITERATIONS)  # same time for unknown users
            return False
        h = self._hash(password, bytes.fromhex(a["salt"]), a["iterations"])
        return hmac.compare_digest(h.hex(), a["hash"])

    def remove(self, username):
        d = self.root / username
        for f in d.iterdir():
            f.unlink()
        d.rmdir()
        self.accounts.pop(username, None)

    def load(self, username, name, default=None):
        f = self.root / username / f"{name}.json"
        return json.loads(f.read_text()) if f.exists() else default

    def save(self, username, name, value):
        (self.root / username / f"{name}.json").write_text(json.dumps(value))

    def clear(self, username, name):
        (self.root / username / f"{name}.json").unlink(missing_ok=True)

    def append_result(self, username, line):
        f = self.root / username / "hist.jsonl"
        lines = f.read_text().splitlines() if f.exists() else []
        lines = (lines + [json.dumps(line, separators=(",", ":"))])[-HISTORY_MAX:]
        f.write_text("\n".join(lines) + "\n")

    def history(self, username, limit):
        f = self.root / username / "hist.jsonl"
        lines = f.read_text().splitlines() if f.exists() else []
        return [json.loads(x) for x in lines[-limit:] if x.strip()]


class Recording:
    """One 10-s golden recording, replayed in a loop as ADC millivolts."""

    def __init__(self, rid, uv, feats, board_gain, rng):
        self.rid, self.uv, self.feats, self.rng = rid, uv, feats, rng
        self.board_gain = board_gain
        y = ef.zero_phase_filter(uv - np.median(uv), ef.design_bandpass(FS), FS)
        self.r_peaks = ef.detect_r_peaks(y, FS)

    def mv(self, n, start, noise=True):
        """n samples of the looping recording from sample index start, as integer mV at GPIO4."""
        idx = (start + np.arange(n)) % len(self.uv)
        v = REF_MV + self.uv[idx] * self.board_gain / 1000.0
        if noise:
            v = v + self.rng.normal(0, ADC_NOISE_MV, n)
        return np.round(np.clip(v, 0, ADC_MAX_MV)).astype(int)


class Device:
    def __init__(self, args):
        self.args, self.rng, self.models = args, np.random.default_rng(args.seed), Models()
        global ADVICE
        ADVICE = self.models.spec["risk_messages"]
        self.rec = self.pick_recording(args)
        self.users = UserStore(Path(args.data))
        self.clients: dict[web.WebSocketResponse, str | None] = {}  # socket -> signed-in username
        self.tokens: dict[str, tuple[str, float]] = {}               # token -> (username, expiry)
        self.t0 = time.monotonic()
        self.sample = 0  # stream position at 500 Hz
        self.gain = {"gain": NOMINAL_GAIN, "calibrated": False, "method": "nominal"}
        self.alert = {"active": False, "kind": "none", "message": ""}
        self.alert_user = None
        self.session = self.session_obj("idle", "measure", 0, 0, 0, "")
        self.session_user = None
        self.sess = None
        self.last_result, self.last_result_user = None, None
        self.result_id = 0
        log.info("accounts in %s (%d users)", args.data, len(self.users.accounts))

    # ------------------------------------------------------------------ setup
    def pick_recording(self, a):
        sig = pd.read_csv(GOLDEN / "signals_uv.csv", header=None, index_col=0)
        feats = pd.read_csv(GOLDEN / "features.csv", index_col=0)
        prof = pd.read_csv(GOLDEN / "profiles.csv", index_col=0)
        scores = pd.read_csv(GOLDEN / "scores.csv", index_col=0)
        ok = feats[feats.sqi_ok == 1].index.intersection(prof.dropna(subset=["age"]).index)
        if a.record:
            rid = a.record
        elif a.risk:
            spec = self.models.info("primary")["tiers_on_score"]
            want = scores.loc[ok, "primary"] >= spec["high"] if a.risk == "high" else scores.loc[ok, "primary"] < spec["elevated"]
            rid = scores.loc[ok][want].index[0]
        else:
            rid = ok[0]
        log.info("replaying golden recording %s (primary score in Python: %.3f)", rid, scores.loc[rid, "primary"])
        return Recording(rid, sig.loc[rid].to_numpy(float), feats.loc[rid], a.board_gain, self.rng)

    # ------------------------------------------------------------------ messages
    def now_ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    @staticmethod
    def session_obj(state, mode, progress, elapsed, duration, message, beats=0, pats=0):
        return {"state": state, "mode": mode, "progress": round(progress, 3), "elapsed_s": round(elapsed, 1),
                "duration_s": duration, "beats": beats, "pats": pats, "message": message}

    def model_info(self):
        ms = {}
        needs = []
        for role in self.models.spec["models"]:
            info = self.models.info(role)
            t = info["tiers_on_score"]
            ms[role] = {"n_inputs": len(info["inputs"]), "non_ecg_inputs": self.models.non_ecg(role),
                        "uses_amplitudes": any(c.endswith("_uv") for c in info["inputs"]),
                        "high": round(self.models.calibrate(role, t["high"]), 4),
                        "elevated": round(self.models.calibrate(role, t["elevated"]), 4)}
            needs += [c for c in self.models.non_ecg(role) if c not in needs]
        return {"version": self.models.spec["version"], "family": "see feature_spec.json",
                "primary_inputs": "ECG + profile", "self_test": True, "window_s": WINDOW_S,
                "min_good_windows": MIN_GOOD, "threshold": ms["primary"]["high"], "needs": needs, "models": ms}

    def hello(self, user):
        m = {"type": "hello", "device": DEVICE, "firmware": FIRMWARE,
             "auth": {"signed_in": user is not None, "users": len(self.users.accounts)},
             "stream": {"ecg_fs": ECG_STREAM_FS, "ecg_unit": "mV, filtered for display", "ppg_fs": PPG_STREAM_FS},
             "model": self.model_info(), "session": {**self.session, "mine": user is not None and user == self.session_user},
             "ecg_gain": self.gain}
        if user is None:
            return m  # personal data only after sign-in
        m["auth"]["username"] = user
        m.update(profile=self.users.load(user, "profile"), calibration=self.users.load(user, "bp", UserStore.DEFAULT_CAL),
                 alert=self.alert if self.alert_user in (None, user) else {"active": False, "kind": "none", "message": ""},
                 last_result=self.last_result if self.last_result_user == user else None)
        return m

    async def send(self, ws, msg):
        if not ws.closed:
            await ws.send_str(json.dumps(msg, separators=(",", ":"), ensure_ascii=False))

    async def send_user(self, user, msg):
        await asyncio.gather(*(self.send(ws, msg) for ws, u in list(self.clients.items()) if u is not None and u == user))

    async def send_signed_in(self, msg):
        await asyncio.gather(*(self.send(ws, msg) for ws, u in list(self.clients.items()) if u is not None))

    def live_to(self, user):
        if user is None:
            return False
        return self.session["state"] != "acquiring" or self.session_user is None or user == self.session_user

    async def send_live(self, msg):
        await asyncio.gather(*(self.send(ws, msg) for ws, u in list(self.clients.items()) if self.live_to(u)))

    async def publish_session(self):
        await asyncio.gather(*(self.send(ws, {"type": "session", **self.session, "mine": u == self.session_user})
                               for ws, u in list(self.clients.items()) if u is not None))

    async def error(self, ws, text):
        await self.send(ws, {"type": "error", "message": text})

    async def set_alert(self, alert, user=None):
        self.alert, self.alert_user = alert, user
        if user is not None:
            await self.send_user(user, {"type": "alert", **alert})
        else:
            await self.send_signed_in({"type": "alert", **alert})

    # ------------------------------------------------------------------ live loop (10 Hz)
    async def run(self):
        tick = 0
        while True:
            await asyncio.sleep(0.1)
            tick += 1
            n = FS // 10
            start = self.sample
            self.sample += n
            ecg = self.rec.mv(n, start)[:: FS // ECG_STREAM_FS].tolist()
            t = (start + np.arange(0, n, FS // PPG_STREAM_FS)) / FS
            ppg = (60000 + 3000 * np.sin(2 * np.pi * 1.1 * t)).astype(int).tolist() if self.args.finger else []
            await self.send_live({"type": "wave", "ecg": ecg, "ppg": ppg})
            for r in self.rec.r_peaks:  # beats inside this 100-ms slice of the loop
                pos = start - start % len(self.rec.uv) + r
                if start <= pos < start + n:
                    await self.send_live({"type": "beat", "t_ms": self.now_ms(), "rr": num(self.rec.feats.mean_rr_s * 1000)})
            if self.sess and self.sess["mode"] == "measure":
                await self.analyse_ready_windows()
            if self.sess and self.elapsed(self.sess) >= self.sess["duration"]:
                await self.advance_session()  # last sample captured: finish at once, not at the next 1-s tick
            elif tick % 10 == 0 and self.sess:
                await self.advance_session()
            if tick % 10 == 0:
                await self.second()

    async def second(self):
        f = self.rec.feats
        hr = float(f.hr_mean_bpm)
        cal = self.users.load(self.session_user, "bp", UserStore.DEFAULT_CAL) if self.session_user else UserStore.DEFAULT_CAL
        v = {"type": "vitals", "t_ms": self.now_ms(), "leads_on": True, "finger_on": self.args.finger,
             "hr": num(hr), "ppg_hr": num(hr) if self.args.finger else None,
             "spo2": 97.0 if self.args.finger else None, "pi": 2.1 if self.args.finger else None,
             "pat_ms": 245.0 if self.args.finger else None, "sbp": 121 if self.args.finger else None,
             "dbp": 78 if self.args.finger else None, "bp_calibrated": cal["calibrated"],
             "sdnn": num(f.sd_rr_s * 1000), "rmssd": num(f.rmssd_ms), "body_temp": 34.2, "room_temp": 24.6,
             "humidity": 55.0, "pressure": 1012.3}
        await self.send_live(v)

    # ------------------------------------------------------------------ sessions
    async def start(self, user, mode, duration, **extra):
        self.session_user = user
        self.sess = {"mode": mode, "t0": time.monotonic(), "duration": duration, "start_sample": self.sample,
                     "user": user, "done": [], **extra}
        await self.advance_session()

    def elapsed(self, s):
        """Session time on the sample clock (like the firmware, the session ends with its last captured sample)."""
        return (self.sample - s["start_sample"]) / FS

    async def analyse_ready_windows(self):
        """Analyse every window whose last sample has been recorded (the firmware's core-0 analysis task)."""
        s = self.sess
        recorded = self.sample - s["start_sample"] - WARMUP_S * FS
        while len(s["done"]) < s["windows"] and recorded >= (len(s["done"]) + 1) * WINDOW_S * FS:
            w = len(s["done"])
            if w == s["windows"] - 1:
                s["capture_done"] = time.perf_counter()  # the last sample is in: result latency starts here
            mv = self.rec.mv(FS * WINDOW_S, s["start_sample"] + (WARMUP_S + w * WINDOW_S) * FS)
            uv = (mv - REF_MV) * 1000.0 / self.gain["gain"]  # the firmware's conversion with the stored gain
            f = ef.device_features(uv, FS)
            s["done"].append(f)
            await self.send_user(s["user"], {"type": "window", "index": w, "of": s["windows"],
                                             "status": "ok" if f["sqi_ok"] else "low_quality", "beats": f["n_beats"],
                                             "hr": num(f["hr_mean_bpm"]), "sqi": bool(f["sqi_ok"])})

    async def advance_session(self):
        s = self.sess
        el = self.elapsed(s)
        msg = "Keep the reference signal connected and steady" if s["mode"] == "gain" else \
            "Keep still, breathe normally and do not talk"
        beats = int(max(0, el - WARMUP_S) * float(self.rec.feats.hr_mean_bpm) / 60)
        if el < s["duration"]:
            self.session = self.session_obj("acquiring", s["mode"], el / s["duration"], el, s["duration"], msg, beats)
            await self.publish_session()
            return
        self.sess = None
        user = s["user"]
        if s["mode"] == "measure":
            self.session = self.session_obj("complete", "measure", 1, s["duration"], s["duration"], "Measurement complete", beats)
            await self.publish_session()
            await self.finish_measurement(s)
        elif s["mode"] == "gain":
            g = self.args.board_gain * (1 + self.rng.normal(0, 0.003))
            self.gain = {"gain": round(g, 1), "calibrated": True, "method": "sine"}
            self.session = self.session_obj("complete", "gain", 1, s["duration"], s["duration"], "Gain measurement complete")
            await self.publish_session()
            await self.send_signed_in({"type": "ecg_gain", **self.gain})
        else:
            cal = {"calibrated": True, "sbp": s["sbp"], "dbp": s["dbp"], "pat_ms": 245.0}
            self.users.save(user, "bp", cal)
            self.session = self.session_obj("complete", "calibrate", 1, s["duration"], s["duration"], "Calibration complete")
            await self.publish_session()
            await self.send_user(user, {"type": "calibration", **cal})

    def non_ecg_inputs(self, user):
        p = self.users.load(user, "profile") if user else None
        if not p:
            return {}
        out = {"age": float(p["age"]), "male_sex": 1.0 if p["gender"] == "male" else 0.0, "BMI": float(p["bmi"])}
        if p.get("smoking") is not None:
            out["smoking"] = 1.0 if p["smoking"] else 0.0
        if p.get("drinking") is not None:
            out["drinking"] = float(p["drinking"])
        return out

    async def finish_measurement(self, s):
        t_capture = s.get("capture_done", time.perf_counter())
        user = s["user"]
        non_ecg = self.non_ecg_inputs(user)
        role = self.models.choose(self.gain["calibrated"], non_ecg)
        windows, scores, summaries = [], [], []
        for f in s["done"]:
            sc = self.models.score(role, f, non_ecg) if role and f["sqi_ok"] else None
            windows.append({"status": "ok" if f["sqi_ok"] else "low_quality", "score": num(sc, 4), "beats": f["n_beats"],
                            "hr": num(f["hr_mean_bpm"]), "beat_corr": num(f["beat_corr"], 3), "sqi": bool(f["sqi_ok"])})
            if sc is not None:
                scores.append(sc)
                summaries.append(f)
        self.result_id += 1
        r = {"type": "result", "id": self.result_id, "t_ms": self.now_ms(), "t": int(time.time()), "model": role or "none",
             "model_version": self.models.spec["version"], "windows_ok": len(scores), "windows_total": len(windows),
             "windows": windows, "warnings": [], "borderline": False}
        if role is None or len(scores) < MIN_GOOD:
            r.update(ok=False, tier="no_result", positive=False, risk=None, score=None, advice=ADVICE["No result"],
                     note="Enter your profile (age, sex, height, weight), or calibrate the ECG gain" if role is None
                     else f"Only {len(scores)} of {len(windows)} ECG windows passed the signal-quality check")
            r["latency_ms"] = int((time.perf_counter() - t_capture) * 1000)
            await self.send_user(user, r)
            return
        score = float(np.median(scores))
        tier = self.models.tier(role, score)
        t = self.models.info(role)["tiers_on_score"]
        med = {k: float(np.nanmedian([f[k] for f in summaries])) for k in ef.EDGE_ALL}
        cal = self.users.load(user, "bp", UserStore.DEFAULT_CAL)
        border = self.models.borderline(role, scores)
        r.update(ok=True, tier=tier, positive=tier == "high", score=round(score, 4),
                 risk=round(self.models.calibrate(role, score), 4),
                 threshold=round(self.models.calibrate(role, t["high"]), 4),
                 threshold_elevated=round(self.models.calibrate(role, t["elevated"]), 4),
                 advice=ADVICE[tier.capitalize()], borderline=border, score_spread=round(max(scores) - min(scores), 4),
                 ecg={"hr": num(med["hr_mean_bpm"]), "rr_sd_ms": num(med["sd_rr_s"] * 1000), "rmssd_ms": num(med["rmssd_ms"]),
                      "pr_ms": num(med["pr_peak_ms"], 0), "qrs_ms": num(med["qrs_mean_ms"], 0), "qt_ms": num(med["qt_peak_ms"], 0),
                      "qtc_ms": num(med["qtc_peak_ms"], 0), "r_uv": num(med["mean_r_uv"], 0), "t_r_ratio": num(med["t_r_ratio"], 3)},
                 inputs={c: num({**med, **non_ecg}.get(c), 3) for c in self.models.info(role)["inputs"]},
                 measurements={"hr": num(med["hr_mean_bpm"]), "sbp": None, "dbp": None, "pat_ms": None,
                               "spo2": 97.0 if self.args.finger else None, "body_temp": 34.2, "room_temp": 24.6,
                               "sdnn": num(med["sd_rr_s"] * 1000), "rmssd": num(med["rmssd_ms"]),
                               "beats": int(sum(w["beats"] for w in windows)), "pats": 0,
                               "bp_calibrated": cal["calibrated"]},
                 gain=self.gain, compute_ms=0)
        if border:
            r["warnings"].append(BORDERLINE_WARNING)
        if role == "fallback":
            r["warnings"].append("The ECG gain is not calibrated, so the gain-invariant fallback model was used (ECG amplitudes ignored)")
        if role == "ecg_only":
            r["warnings"].append("No profile entered: the ECG-only model was used, which is less accurate than the model with age, sex and BMI")
        r["latency_ms"] = int((time.perf_counter() - t_capture) * 1000)
        self.last_result, self.last_result_user = r, user
        self.users.append_result(user, {
            "id": r["id"], "t": r["t"], "tier": tier, "model": role, "risk": r["risk"], "score": r["score"], "borderline": border,
            "windows_ok": r["windows_ok"], "windows_total": r["windows_total"], "hr": r["ecg"]["hr"], "qtc_ms": r["ecg"]["qtc_ms"],
            "rmssd_ms": r["ecg"]["rmssd_ms"], "sbp": None, "dbp": None, "spo2": r["measurements"]["spo2"], "latency_ms": r["latency_ms"]})
        await self.send_user(user, r)
        if tier == "high":
            await self.set_alert({"active": True, "kind": "risk",
                                  "message": f"Raised risk of prediabetes or diabetes ({100 * r['risk']:.0f} %). Please visit a "
                                             "clinic or hospital for a fasting glucose or HbA1c blood test."}, user)
        log.info("result #%d for %s: model %s, score %.4f, risk %.3f, tier %s%s, %d ms after the recording", r["id"], user,
                 role, score, r["risk"], tier, " (borderline)" if border else "", r["latency_ms"])

    # ------------------------------------------------------------------ commands
    async def sign_in(self, ws, user, token):
        self.clients[ws] = user
        await self.send(ws, {"type": "auth", "ok": True, "username": user, "token": token})
        await self.send(ws, self.hello(user))

    async def command(self, ws, text):
        try:
            c = json.loads(text)
            cmd = c["cmd"]
        except (ValueError, KeyError, TypeError):
            return await self.error(ws, "Command is not a JSON object")
        user = self.clients.get(ws)
        if cmd == "hello":
            return await self.send(ws, self.hello(user))
        if cmd in ("register", "login"):
            name, pw = str(c.get("username", "")).strip().lower(), str(c.get("password", ""))
            if cmd == "register":
                err = self.users.create(name, pw)
                if err:
                    return await self.send(ws, {"type": "auth", "ok": False, "message": err})
                log.info("account %s created", name)
            elif not self.users.verify(name, pw):
                return await self.send(ws, {"type": "auth", "ok": False, "message": "Wrong username or password"})
            token = secrets.token_hex(16)
            self.tokens[token] = (name, time.time() + SESSION_TTL_S)
            return await self.sign_in(ws, name, token)
        if cmd == "auth":
            token = str(c.get("token", ""))
            name, exp = self.tokens.get(token, (None, 0))
            if not name or exp < time.time() or name not in self.users.accounts:
                return await self.send(ws, {"type": "auth", "ok": False, "message": "Please sign in again"})
            self.tokens[token] = (name, time.time() + SESSION_TTL_S)
            return await self.sign_in(ws, name, token)
        if user is None:
            return await self.send(ws, {"type": "auth", "ok": False, "message": "Please sign in"})

        busy = self.sess is not None
        if cmd == "logout":
            self.clients[ws] = None
            self.tokens = {t: v for t, v in self.tokens.items() if not (v[0] == user and t == c.get("token"))}
            await self.send(ws, {"type": "auth", "ok": False, "message": "Signed out"})
        elif cmd == "delete_account":
            if not self.users.verify(user, str(c.get("password", ""))):
                return await self.error(ws, "Wrong password: the account was not deleted")
            if busy and self.session_user == user:
                self.sess = None
                self.session = self.session_obj("idle", "measure", 0, 0, 0, "")
            self.users.remove(user)
            self.tokens = {t: v for t, v in self.tokens.items() if v[0] != user}
            if self.last_result_user == user:
                self.last_result, self.last_result_user = None, None
            for other, u in list(self.clients.items()):
                if u == user:
                    self.clients[other] = None
                    await self.send(other, {"type": "auth", "ok": False, "message": "Account deleted"})
            log.info("account %s deleted", user)
        elif cmd == "history":
            limit = max(1, min(int(c.get("limit") or 60), HISTORY_MAX))
            await self.send(ws, {"type": "history", "results": self.users.history(user, limit)})
        elif cmd == "start":
            if busy:
                return await self.error(ws, "The device is busy with another measurement")
            d = int(c.get("duration_s") or WARMUP_S + 3 * WINDOW_S)
            if not WARMUP_S + MIN_WINDOWS * WINDOW_S <= d <= WARMUP_S + MAX_WINDOWS * WINDOW_S:
                return await self.error(ws, "duration_s must be between 23 and 63")
            if self.models.choose(self.gain["calibrated"], self.non_ecg_inputs(user)) is None:
                return await self.error(ws, "Enter your profile (age, sex, height, weight), or calibrate the ECG gain to use the ECG-only model")
            windows = max(MIN_WINDOWS, min(MAX_WINDOWS, (d - WARMUP_S) // WINDOW_S))
            if self.alert["kind"] == "risk":
                await self.set_alert({"active": False, "kind": "none", "message": ""})
            await self.start(user, "measure", WARMUP_S + windows * WINDOW_S, windows=windows)
        elif cmd == "cancel":
            if busy and self.session_user != user:
                return await self.error(ws, "Only the person who started the measurement can cancel it")
            if busy:
                self.sess = None
                self.session = self.session_obj("idle", "measure", 0, 0, 0, "Measurement cancelled")
                await self.publish_session()
        elif cmd == "ack":
            await self.set_alert({"active": False, "kind": "none", "message": ""})
        elif cmd == "profile":
            try:
                age, h, w = float(c["age"]), float(c["height_cm"]), float(c["weight_kg"])
                assert c["gender"] in ("female", "male") and 18 <= age <= 100 and 120 <= h <= 220 and 25 <= w <= 250
            except (KeyError, ValueError, TypeError, AssertionError):
                return await self.error(ws, "Invalid profile (age 18-100, gender female/male, height 120-220 cm, weight 25-250 kg)")
            prof = {"age": age, "gender": c["gender"], "height_cm": h, "weight_kg": w,
                    "bmi": round(w / (h / 100) ** 2, 1), "smoking": c.get("smoking"), "drinking": c.get("drinking"),
                    "persist": c.get("persist", True)}
            self.users.save(user, "profile", prof)
            await self.send_user(user, {"type": "profile", **prof})
        elif cmd == "calibrate":
            if busy:
                return await self.error(ws, "The device is busy with another measurement")
            await self.start(user, "calibrate", BP_CAL_S, sbp=c.get("sbp"), dbp=c.get("dbp"))
        elif cmd == "clear_calibration":
            self.users.clear(user, "bp")
            await self.send_user(user, {"type": "calibration", **UserStore.DEFAULT_CAL})
        elif cmd == "calibrate_gain":
            if busy:
                return await self.error(ws, "The device is busy with another measurement")
            await self.start(user, "gain", GAIN_CAL_S)
        elif cmd == "set_gain":
            g = c.get("gain")
            if not isinstance(g, (int, float)) or not 500 <= g <= 2000:
                return await self.error(ws, "gain must be 500-2000")
            self.gain = {"gain": float(g), "calibrated": True, "method": "entered"}
            await self.send_signed_in({"type": "ecg_gain", **self.gain})
        elif cmd == "clear_gain":
            self.gain = {"gain": NOMINAL_GAIN, "calibrated": False, "method": "nominal"}
            await self.send_signed_in({"type": "ecg_gain", **self.gain})
        else:
            await self.error(ws, "Unknown command")

    # ------------------------------------------------------------------ HTTP
    async def ws_handler(self, request):
        ws = web.WebSocketResponse(heartbeat=20)
        await ws.prepare(request)
        self.clients[ws] = None
        log.info("client connected (%d)", len(self.clients))
        await self.send(ws, self.hello(None))
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    await self.command(ws, msg.data)
        finally:
            self.clients.pop(ws, None)
        return ws

    async def index(self, _):
        return web.FileResponse(INDEX_HTML, headers={"Cache-Control": "no-store"})

    async def state(self, _):
        return web.json_response({"device": DEVICE, "firmware": FIRMWARE, "model_version": self.models.spec["version"],
                                  "users": len(self.users.accounts), "session": "busy" if self.sess else "idle",
                                  "self_test": True}, headers={"Access-Control-Allow-Origin": "*"})

    async def captive(self, request):
        """OS connectivity probes are answered with a redirect to the sign-in page (captive portal)."""
        raise web.HTTPFound(f"http://{request.host}/")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--risk", choices=["high", "low"], help="replay a recording with a high or low primary score")
    ap.add_argument("--record", help="golden recording id to replay (firmware/test/golden/features.csv)")
    ap.add_argument("--board-gain", type=float, default=NOMINAL_GAIN, help="true gain of the simulated AD8232 board")
    ap.add_argument("--no-finger", dest="finger", action="store_false", help="simulate no finger on the MAX30102")
    ap.add_argument("--data", default=str(Path(tempfile.gettempdir()) / "diagnostix-mock-users"),
                    help="directory for the simulated accounts and per-user data")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s", datefmt="%H:%M:%S")
    dev = Device(args)
    app = web.Application()
    app.add_routes([web.get("/", dev.index), web.get("/ws", dev.ws_handler), web.get("/api/state", dev.state)] +
                   [web.get(p, dev.captive) for p in CAPTIVE_PROBES])

    async def start_loop(app_):
        app_["loop_task"] = asyncio.create_task(dev.run())

    app.on_startup.append(start_loop)
    log.info("dashboard: http://localhost:%d/  (WebSocket ws://<this computer>:%d/ws)", args.port, args.port)
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
