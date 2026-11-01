#!/usr/bin/env python3
"""Banc d'acquisition et analyse EchoLabNT. Aucune simulation d'effet Python."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import wave
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
STATUSES = {"documented", "binary_observed", "measured", "inferred", "provisional", "unknown", "synthetic_fixture"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_wav(path):
    """RIFF little-endian PCM 8/16/24/32 bits ou IEEE float32/64, sans normalisation."""
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("WAV RIFF little-endian requis")
    declared = struct.unpack_from("<I", data, 4)[0] + 8
    if declared > len(data):
        raise ValueError("WAV tronqué (taille RIFF)")
    fmt = payload = None
    pos = 12
    while pos + 8 <= declared:
        kind, size = struct.unpack_from("<4sI", data, pos)
        pos += 8
        if pos + size > declared:
            raise ValueError("WAV tronqué (chunk)")
        if kind == b"fmt ":
            fmt = data[pos:pos + size]
        elif kind == b"data":
            if payload is not None:
                raise ValueError("Plusieurs chunks data non supportés")
            payload = data[pos:pos + size]
        pos += size + (size & 1)
    if fmt is None or len(fmt) < 16 or payload is None:
        raise ValueError("WAV sans fmt/data valide")
    code, channels, fs, byte_rate, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if code == 0xFFFE and len(fmt) >= 40:
        # WAVEFORMATEXTENSIBLE : GUID PCM/IEEE conventionnel uniquement.
        if fmt[26:40] != bytes.fromhex("000000001000800000aa00389b71"):
            raise ValueError("Sous-format WAV extensible inconnu")
        code = struct.unpack_from("<H", fmt, 24)[0]
    if channels < 1 or channels > 64 or fs < 1 or bits not in (8, 16, 24, 32, 64) or align <= 0 or align != channels * (bits // 8) or byte_rate != fs * align:
        raise ValueError("Format WAV incohérent")
    if len(payload) % align:
        raise ValueError("WAV tronqué (frame)")
    if code == 1 and bits == 8:
        samples = (np.frombuffer(payload, np.uint8).astype(np.float64) - 128) / 128
    elif code == 1 and bits in (16, 32):
        samples = np.frombuffer(payload, "<i2" if bits == 16 else "<i4").astype(np.float64) / (2 ** (bits - 1))
    elif code == 1 and bits == 24:
        packed = np.frombuffer(payload, np.uint8).reshape(-1, 3).astype(np.int32)
        words = packed[:, 0] | packed[:, 1] << 8 | packed[:, 2] << 16
        words = (words ^ 0x800000) - 0x800000
        samples = words.astype(np.float64) / 8388608
    elif code == 3 and bits in (32, 64):
        samples = np.frombuffer(payload, "<f4" if bits == 32 else "<f8").astype(np.float64)
    else:
        raise ValueError(f"Format WAV non supporté : code={code}, bits={bits}")
    return samples.reshape(-1, channels), fs, {"format_code": code, "bits": bits, "channels": channels}


def write_wav(path, samples, fs):
    samples = np.asarray(samples, dtype=np.float64)
    if samples.ndim == 1:
        samples = samples[:, None]
    if not np.all(np.isfinite(samples)) or np.max(np.abs(samples), initial=0) > 1:
        raise ValueError("Échantillons non finis ou amplitude >1 ; aucun clipping implicite")
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    # PCM24 ; quantification explicite, aucun changement de gain.
    ints = np.rint(np.clip(samples, -1, 1 - 1 / 8388608) * 8388608).astype(np.int32).reshape(-1)
    packed = np.column_stack((ints & 255, ints >> 8 & 255, ints >> 16 & 255)).astype(np.uint8)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(samples.shape[1])
        output.setsampwidth(3)
        output.setframerate(fs)
        output.writeframes(packed.tobytes())


def base_metadata(command, paths=(), status="measured", configuration=None):
    try:
        revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                  capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        revision = None
    return {"schema_version": 1, "command": command, "status": status,
            "created_utc": datetime.now(timezone.utc).isoformat(),
            "tool": "tools/analysis/echo_tools.py", "tool_sha256": digest(__file__),
            "git_revision": revision, "numpy_version": np.__version__,
            "input_class": "synthetic_fixture" if status == "synthetic_fixture" else "user_supplied_unqualified",
            "parents": [{"path": str(Path(p)), "sha256": digest(p)} for p in paths],
            "configuration": configuration or {}, "resampling": None,
            "gain_normalization": None, "removed_delay_samples": 0,
            "excluded_regions": [], "ir_role": "unknown"}


def save_json(path, obj):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def assert_finite(samples):
    if not np.all(np.isfinite(samples)):
        raise ValueError("Le signal contient NaN/Inf")


def load_channel(path, channel=0):
    x, fs, _ = read_wav(path)
    assert_finite(x)
    if channel < 0 or channel >= x.shape[1] or len(x) == 0:
        raise ValueError("Canal absent ou WAV vide")
    return x[:, channel], fs


def generate_stimuli(args):
    if args.sample_rate < 8000 or args.sample_rate > 384000 or args.duration <= 0 or args.duration > 300:
        raise ValueError("Fs 8..384 kHz et durée ]0,300] s requises")
    if args.level_dbfs > -1 or args.level_dbfs < -120:
        raise ValueError("Niveau requis entre -120 et -1 dBFS")
    fs = args.sample_rate
    n = round(args.duration * fs)
    t = np.arange(n) / fs
    amplitude = 10 ** (args.level_dbfs / 20)
    lead = round(args.lead_seconds * fs)
    if lead < 0 or lead >= n:
        raise ValueError("Pré-silence hors durée")
    active = np.maximum(t - args.lead_seconds, 0)
    if args.kind == "impulse":
        signal = np.zeros(n)
        signal[lead] = amplitude
    elif args.kind == "sine":
        if not 0 < args.frequency < fs / 2:
            raise ValueError("Fréquence hors bande")
        signal = amplitude * np.sin(2 * np.pi * args.frequency * active)
        signal[:lead] = 0
    elif args.kind == "sweep":
        duration = args.duration - args.lead_seconds
        if not 0 < args.start_hz < args.end_hz < fs / 2:
            raise ValueError("Balayage : 0 < début < fin < Nyquist")
        rate = math.log(args.end_hz / args.start_hz) / duration
        signal = amplitude * np.sin(2 * np.pi * args.start_hz * np.expm1(active * rate) / rate)
        signal[:lead] = 0
        # 10 ms raised-cosine endpoints; the inverse includes this taper through regularized inversion.
        fade = min(round(0.01 * fs), (n - lead) // 2)
        if fade:
            ramp = np.sin(np.linspace(0, np.pi / 2, fade)) ** 2
            signal[lead:lead + fade] *= ramp
            signal[-fade:] *= ramp[::-1]
    elif args.kind == "multitone":
        frequencies = [float(value) for value in args.frequencies.split(",")]
        if not frequencies or any(f <= 0 or f >= fs / 2 for f in frequencies):
            raise ValueError("Fréquences multitone hors bande")
        # Deterministic Schroeder phases; amplitude bound is sum-normalized at construction.
        signal = sum(np.sin(2 * np.pi * f * active + math.pi * k * (k - 1) / len(frequencies))
                     for k, f in enumerate(frequencies)) * amplitude / len(frequencies)
        signal[:lead] = 0
    else:
        # Asymmetric identifiable transients, for Time/reverse/state protocols.
        signal = np.zeros(n)
        for seconds, gain in [(0, 1), (.137, -.5), (.419, .25), (.913, .75)]:
            index = lead + round(seconds * fs)
            if index < n:
                signal[index] = amplitude * gain
    stereo = np.zeros((n, args.channels))
    stereo[:, args.active_channel] = signal
    write_wav(args.output, stereo, fs)
    meta = base_metadata("generate_stimuli", status="synthetic_fixture", configuration=vars_clean(args))
    meta.update({"file": str(args.output), "sha256": digest(args.output), "sample_rate": fs,
                 "frames": n, "sample_format": "PCM24", "peak_limit_dbfs": args.level_dbfs,
                 "purpose": "excitation synthétique, aucune capture Line 6"})
    if args.kind == "sweep":
        # Store an exact frequency-domain inverse, not a silently normalized time-reversed sweep.
        # Valid only on the documented excited band; first-pass uses the same regularization.
        fft_size = 1 << (2 * n - 1).bit_length()
        spectrum = np.fft.rfft(signal, fft_size)
        power = np.abs(spectrum) ** 2
        floor = float(power.max(initial=0)) * 1e-10
        frequencies = np.fft.rfftfreq(fft_size, 1 / fs)
        mask = (frequencies >= args.start_hz) & (frequencies <= args.end_hz)
        inverse = np.where(mask, np.conj(spectrum) / (power + floor), 0)
        inverse_path = args.output.with_suffix(".inverse.npz")
        np.savez(inverse_path, inverse_real=inverse.real, inverse_imag=inverse.imag,
                 sample_rate=fs, fft_size=fft_size, regularization=floor,
                 band_hz=np.array([args.start_hz, args.end_hz]))
        meta["inverse"] = {"path": str(inverse_path), "sha256": digest(inverse_path),
                           "method": "conj(X)/(abs(X)^2+lambda), zero hors bande excitée",
                           "fft_size": fft_size, "regularization_absolute": floor,
                           "includes_taper": True, "harmonic_separation": "not_implemented"}
    save_json(args.output.with_suffix(".json"), meta)
    return meta


def vars_clean(args):
    return {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()
            if key not in ("func", "diagnostic")}


def observation_status(args, default="measured"):
    return "synthetic_fixture" if getattr(args, "synthetic_fixture", False) else default


def protect_inputs(args):
    """Validate every intended output before processing; raw and parent files are read-only contracts."""
    inputs = [getattr(args, key) for key in ("capture", "excitation", "reference", "candidate", "manifest")
              if getattr(args, key, None) is not None]
    parents = {Path(path).resolve() for path in inputs}
    for key, value in vars(args).items():
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError(f"Valeur numérique non finie : {key}")
    outputs = [args.output]
    if args.command == "generate_stimuli":
        outputs.extend([args.output.with_suffix(".json"), args.output.with_suffix(".inverse.npz")])
    elif args.command in ("estimate_first_pass", "estimate_loop"):
        outputs.append(args.output.with_suffix(".npz"))
    raw = (ROOT / "reference/raw").resolve()
    for output in outputs:
        resolved = output.resolve()
        if resolved in parents:
            raise ValueError(f"La sortie remplacerait un fichier parent : {output}")
        if resolved == raw or raw in resolved.parents:
            raise ValueError(f"Dérivé interdit dans raw immuable : {output}")
    if len({output.resolve() for output in outputs}) != len(outputs):
        raise ValueError("Collision entre sorties générées")
    if args.command == "generate_stimuli" and args.output.suffix.lower() != ".wav":
        raise ValueError("Le stimulus doit utiliser l'extension .wav (métadonnées .json séparées)")


def validate_capture(args):
    x, fs, info = read_wav(args.capture)
    issues = []
    finite = bool(np.all(np.isfinite(x)))
    if not finite:
        issues.append("non_finite")
    safe = np.where(np.isfinite(x), x, 0)
    peak = np.max(np.abs(safe), axis=0, initial=0)
    rms = np.sqrt(np.mean(safe ** 2, axis=0)) if len(x) else np.zeros(x.shape[1])
    clipping = np.sum(np.abs(safe) >= 1 - 1 / (2 ** (info["bits"] - 1)), axis=0)
    if len(x) == 0:
        issues.append("empty")
    if np.any(clipping):
        issues.append("possible_clipping")
    if np.any(peak == 0):
        issues.append("silent_channel")
    manifest = None
    if args.manifest:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        required = ["id", "capture", "excitation", "profile", "sample_rates", "parameters", "channels",
                    "gains", "calibration", "routing", "initial_state", "events", "repetitions", "anomalies", "status", "ir_role"]
        issues.extend("missing_metadata:" + key for key in required if key not in manifest)
        if manifest.get("status") not in STATUSES:
            issues.append("invalid_status")
        if manifest.get("ir_role") not in ("calibration", "first_pass", "loop_increment", "full_wet_response", "unknown"):
            issues.append("invalid_ir_role")
        expected = manifest.get("capture", {}).get("sha256")
        if expected is not None and expected != digest(args.capture):
            issues.append("capture_sha256_mismatch")
        file_fs = manifest.get("sample_rates", {}).get("capture_file_hz")
        if file_fs is not None and file_fs != fs:
            issues.append("capture_sample_rate_mismatch")
    noise = None
    if args.noise_start is not None or args.noise_end is not None:
        if args.noise_start is None or args.noise_end is None or not 0 <= args.noise_start < args.noise_end <= len(x) / fs:
            raise ValueError("Fenêtre de bruit explicite invalide")
        region = safe[round(args.noise_start * fs):round(args.noise_end * fs)]
        noise_rms = np.sqrt(np.mean(region ** 2, axis=0))
        noise = {"window_seconds": [args.noise_start, args.noise_end], "rms": noise_rms.tolist(),
                 "signal_to_noise_db": [float(20 * np.log10(r / nr)) if r > 0 and nr > 0 else None for r, nr in zip(rms, noise_rms)],
                 "qualification": "rapport RMS total / fenêtre déclarée silencieuse ; vérifier cette hypothèse"}
    meta = base_metadata("validate_capture", [args.capture] + ([args.manifest] if args.manifest else []),
                         status=observation_status(args, manifest.get("status", "unknown") if manifest else "unknown"), configuration=vars_clean(args))
    meta.update({"sample_rate": fs, "frames": len(x), "duration_seconds": len(x) / fs, "format": info,
                 "peak_absolute": peak.tolist(), "rms_absolute": rms.tolist(), "clipping_samples": clipping.tolist(),
                 "finite": finite, "issues": issues, "noise": noise,
                 "qualification": "inspection du fichier ; saturation analogique/état initial/répétabilité inconnus",
                 "valid": not issues})
    save_json(args.output, meta)
    return meta


def window_region(x, fs, start, end):
    if not 0 <= start < end <= len(x) / fs + 0.5 / fs:
        raise ValueError("Fenêtre hors du signal")
    return x[round(start * fs):round(end * fs)]


def estimate_first_pass(args):
    excitation, fs = load_channel(args.excitation, args.input_channel)
    capture, capture_fs = load_channel(args.capture, args.output_channel)
    if fs != capture_fs:
        raise ValueError("Fs différentes : rééchantillonnage explicite requis")
    y = window_region(capture, fs, args.start, args.end)
    x = excitation
    if args.regularization <= 0 or not 0 <= args.min_hz < args.max_hz <= fs / 2:
        raise ValueError("Régularisation positive et bande explicite requises")
    nfft = 1 << (len(x) + len(y) - 1).bit_length()
    X = np.fft.rfft(x, nfft)
    Y = np.fft.rfft(y, nfft)
    power = np.abs(X) ** 2
    if power.max(initial=0) == 0:
        raise ValueError("Excitation muette")
    frequency = np.fft.rfftfreq(nfft, 1 / fs)
    excited = power >= power.max() * args.excitation_floor
    mask = (frequency >= args.min_hz) & (frequency <= args.max_hz) & excited
    H = np.where(mask, Y * np.conj(X) / (power + args.regularization * power.max()), 0)
    impulse = np.fft.irfft(H, nfft)
    npz = args.output.with_suffix(".npz")
    np.savez(npz, frequency_hz=frequency, transfer_real=H.real, transfer_imag=H.imag,
             response=impulse, excited_mask=mask, sample_rate=fs)
    meta = base_metadata("estimate_first_pass", [args.excitation, args.capture], status=observation_status(args), configuration=vars_clean(args))
    meta.update({"ir_role": args.ir_role, "derived": str(npz), "derived_sha256": digest(npz),
                 "window_capture_seconds": [args.start, args.end], "window_type": "rectangular",
                 "time_origin_seconds": args.start, "nominal_delay_removed_seconds": args.start,
                 "fft_size": nfft, "regularization_absolute": float(args.regularization * power.max()),
                 "excited_bin_fraction": float(np.mean(mask)), "sample_rate": fs,
                 "assumptions": ["LTI locale à vérifier par niveaux et prises répétées", "fenêtre isolant la réponse visée", "aucune séparation harmonique"],
                 "qualification": "transfert observé ; aucune attribution automatique à un modèle Line 6"})
    meta["removed_delay_samples"] = round(args.start * fs)
    save_json(args.output, meta)
    return meta


def estimate_loop(args):
    x, fs = load_channel(args.capture, args.channel)
    if args.topology != "single_path":
        raise ValueError("Chemins couplés/têtes : estimation matricielle ou ajustement structurel requis")
    starts = [float(s) for s in args.echo_starts.split(",")]
    if len(starts) < 3 or starts != sorted(set(starts)) or args.regularization <= 0:
        raise ValueError("Au moins 3 débuts d'échos distincts croissants et régularisation positive requis")
    if any(b - a < args.window for a, b in zip(starts, starts[1:])):
        raise ValueError("Fenêtres d'échos superposées")
    echoes = [window_region(x, fs, s, s + args.window) for s in starts]
    length = min(map(len, echoes))
    nfft = 1 << max(1, 2 * length - 1).bit_length()
    spectra = [np.fft.rfft(e[:length], nfft) for e in echoes]
    estimates, gains = [], []
    for current, following in zip(spectra, spectra[1:]):
        power = np.abs(current) ** 2
        if power.max(initial=0) == 0:
            raise ValueError("Écho muet ; transfert non identifiable")
        estimates.append(following * np.conj(current) / (power + args.regularization * power.max()))
    for current, following in zip(echoes, echoes[1:]):
        energy = float(np.dot(current[:length], current[:length]))
        gains.append(float(np.dot(current[:length], following[:length]) / energy) if energy else None)
    values = np.array(estimates)
    output = args.output.with_suffix(".npz")
    np.savez(output, frequency_hz=np.fft.rfftfreq(nfft, 1 / fs), transfer_real=values.real,
             transfer_imag=values.imag, mean_real=values.mean(axis=0).real,
             mean_imag=values.mean(axis=0).imag, magnitude_std=np.abs(values).std(axis=0), sample_rate=fs)
    meta = base_metadata("estimate_loop", [args.capture], status=observation_status(args), configuration=vars_clean(args))
    meta.update({"ir_role": "loop_increment", "derived": str(output), "derived_sha256": digest(output),
                 "echo_starts_seconds": starts, "removed_nominal_intervals_seconds": np.diff(starts).tolist(),
                 "window_seconds": args.window, "window_type": "rectangular", "sample_rate": fs,
                 "scalar_pair_gains": gains, "pair_gain_mean": float(np.mean(gains)),
                 "pair_gain_std": float(np.std(gains)), "pair_count": len(gains),
                 "regularization": "relative à la puissance max de chaque écho, choisie par utilisateur selon bruit",
                 "assumptions": ["opérateur scalaire unique", "échos séparés", "faible niveau et invariance à vérifier"],
                 "qualification": "transfert effectif, factorisation/ordre des filtres non identifiés ; aucun réalignement libre"})
    save_json(args.output, meta)
    return meta


def compare_render(args):
    reference, fs, _ = read_wav(args.reference)
    candidate, other_fs, _ = read_wav(args.candidate)
    assert_finite(reference)
    assert_finite(candidate)
    if fs != other_fs or reference.shape[1] != candidate.shape[1]:
        raise ValueError("Fs ou nombre de canaux différents")
    if args.delay_samples > 0:
        candidate = candidate[args.delay_samples:]
    elif args.delay_samples < 0:
        reference = reference[-args.delay_samples:]
    n = min(len(reference), len(candidate))
    if n == 0:
        raise ValueError("Aucun échantillon comparable")
    r, c = reference[:n], candidate[:n]
    error = c - r
    energy = np.sum(r ** 2, axis=0)
    mse = np.mean(error ** 2, axis=0)
    gains = np.divide(np.sum(c * r, axis=0), energy, out=np.zeros_like(energy), where=energy > 0)
    meta = base_metadata("compare_render", [args.reference, args.candidate], status=observation_status(args), configuration=vars_clean(args))
    meta.update({"sample_rate": fs, "compared_frames": n, "reference_frames": len(reference),
                 "candidate_frames": len(candidate), "rmse_absolute": np.sqrt(mse).tolist(),
                 "max_error_absolute": np.max(np.abs(error), axis=0).tolist(), "diagnostic_linear_gain": gains.tolist(),
                 "residual_db_relative": [float(10 * np.log10(e * n / en)) if e > 0 and en > 0 else None for e, en in zip(mse, energy)],
                 "exact_match": bool(np.array_equal(r, c) and len(reference) == len(candidate)),
                 "qualification": "gain absolu préservé ; aucune normalisation, aucun ajustement automatique ; seuil de fidélité non qualifié"})
    meta["removed_delay_samples"] = args.delay_samples
    save_json(args.output, meta)
    return meta


def calibrate(args):
    x, fs = load_channel(args.excitation, args.channel)
    y, other_fs = load_channel(args.capture, args.channel)
    if fs != other_fs:
        raise ValueError("Fs différentes")
    if args.max_delay_seconds < 0:
        raise ValueError("Retard maximum négatif")
    # Linear FFT correlation bounded to acquisition delay >=0; no resampling or drift compensation.
    nfft = 1 << (len(x) + len(y) - 1).bit_length()
    cross = np.fft.irfft(np.fft.rfft(y, nfft) * np.conj(np.fft.rfft(x, nfft)), nfft)
    limit = min(len(y), round(args.max_delay_seconds * fs) + 1)
    if limit <= 0 or np.dot(x, x) == 0:
        raise ValueError("Excitation vide/muette")
    delay = int(np.argmax(np.abs(cross[:limit])))
    n = min(len(x), len(y) - delay)
    gain = float(np.dot(x[:n], y[delay:delay + n]) / np.dot(x[:n], x[:n]))
    residual = y[delay:delay + n] - gain * x[:n]
    meta = base_metadata("calibrate", [args.excitation, args.capture], status=observation_status(args), configuration=vars_clean(args))
    meta.update({"ir_role": "calibration", "sample_rate": fs, "delay_samples": delay,
                 "delay_seconds": delay / fs, "linear_gain": gain,
                 "residual_rms": float(np.sqrt(np.mean(residual ** 2))),
                 "applied_to_files": False, "qualification": "calibration gain/retard entier ; coloration et dérive non corrigées"})
    save_json(args.output, meta)
    return meta


def inventory_corpus(args):
    records = []
    for p in sorted(args.directory.rglob("*")):
        if p.is_file() and p.suffix.lower() in (".wav", ".aiff", ".aif", ".flac"):
            record = {"path": p.relative_to(args.directory).as_posix(), "sha256": digest(p),
                      "bytes": p.stat().st_size, "status": "unknown"}
            if p.suffix.lower() == ".wav":
                try:
                    samples, fs, info = read_wav(p)
                    record.update({"sample_rate": fs, "frames": len(samples), "format": info})
                except ValueError as error:
                    record["error"] = str(error)
            records.append(record)
    meta = base_metadata("inventory_corpus", configuration=vars_clean(args), status=observation_status(args, "documented"))
    meta.update({"audio_files": records, "count": len(records), "qualification": "inventaire ; aucun fichier promu en référence sonore"})
    save_json(args.output, meta)
    return meta


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--diagnostic", action="store_true", help="Afficher la trace Python en cas d'erreur")
    sub = p.add_subparsers(dest="command", required=True)
    q = sub.add_parser("generate_stimuli", help="Excitations PCM24 et manifeste, jamais envoyées au matériel")
    q.add_argument("--kind", choices=["impulse", "sine", "sweep", "multitone", "transition"], default="impulse")
    q.add_argument("--sample-rate", type=int, default=48000)
    q.add_argument("--duration", type=float, default=4)
    q.add_argument("--lead-seconds", type=float, default=.25)
    q.add_argument("--level-dbfs", type=float, default=-18)
    q.add_argument("--frequency", type=float, default=1000)
    q.add_argument("--start-hz", type=float, default=20)
    q.add_argument("--end-hz", type=float, default=20000)
    q.add_argument("--frequencies", default="137,503,1103,3001,7001")
    q.add_argument("--channels", type=int, choices=[1, 2], default=1)
    q.add_argument("--active-channel", type=int, choices=[0, 1], default=0)
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=generate_stimuli)
    q = sub.add_parser("validate_capture", help="Clipping, canaux, métadonnées, hash et bruit explicite")
    q.add_argument("capture", type=Path)
    q.add_argument("--manifest", type=Path)
    q.add_argument("--noise-start", type=float)
    q.add_argument("--noise-end", type=float)
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=validate_capture)
    q = sub.add_parser("estimate_first_pass", help="Déconvolution régularisée, bande explicite, gain et origine conservés")
    q.add_argument("excitation", type=Path)
    q.add_argument("capture", type=Path)
    q.add_argument("--input-channel", type=int, default=0)
    q.add_argument("--output-channel", type=int, default=0)
    q.add_argument("--start", type=float, required=True)
    q.add_argument("--end", type=float, required=True)
    q.add_argument("--min-hz", type=float, required=True)
    q.add_argument("--max-hz", type=float, required=True)
    q.add_argument("--regularization", type=float, required=True, help="Puissance relative, choisie selon bruit/répétabilité")
    q.add_argument("--excitation-floor", type=float, default=1e-8)
    q.add_argument("--ir-role", choices=["calibration", "first_pass", "full_wet_response", "unknown"], default="first_pass")
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=estimate_first_pass)
    q = sub.add_parser("estimate_loop", help="Transfert effectif entre échos isolés ; chemins scalaires seulement")
    q.add_argument("capture", type=Path)
    q.add_argument("--channel", type=int, default=0)
    q.add_argument("--echo-starts", required=True, help="Débuts nominaux en secondes, séparés par virgule")
    q.add_argument("--window", type=float, required=True)
    q.add_argument("--regularization", type=float, required=True)
    q.add_argument("--topology", choices=["single_path", "coupled"], required=True)
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=estimate_loop)
    q = sub.add_parser("compare_render", help="Comparaison absolue, sans ajustement automatique de gain/temps")
    q.add_argument("reference", type=Path)
    q.add_argument("candidate", type=Path)
    q.add_argument("--delay-samples", type=int, default=0, help="Retrait explicite en tête du candidat si positif")
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=compare_render)
    q = sub.add_parser("calibrate", help="Estimer gain/retard entier sans modifier les prises")
    q.add_argument("excitation", type=Path)
    q.add_argument("capture", type=Path)
    q.add_argument("--channel", type=int, default=0)
    q.add_argument("--max-delay-seconds", type=float, default=1)
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=calibrate)
    q = sub.add_parser("inventory_corpus", help="Inventaire audio, sans qualifier la provenance")
    q.add_argument("directory", type=Path)
    q.add_argument("--output", type=Path, required=True)
    q.set_defaults(func=inventory_corpus)
    for command, command_parser in sub.choices.items():
        if command != "generate_stimuli":
            command_parser.add_argument("--synthetic-fixture", action="store_true", help="Marquer les dérivés d'un test synthétique, jamais comme référence produit")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        protect_inputs(args)
        if args.command == "generate_stimuli" and args.active_channel >= args.channels:
            raise ValueError("Canal actif absent")
        result = args.func(args)
        print(json.dumps({"command": args.command, "status": result["status"], "output": str(args.output)}, ensure_ascii=False))
        return 1 if args.command == "validate_capture" and not result["valid"] else 0
    except (ValueError, OSError, struct.error, json.JSONDecodeError) as error:
        if args.diagnostic:
            raise
        print(f"Analyse impossible : {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
