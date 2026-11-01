#!/usr/bin/env python3
"""Preuves structurelles indépendantes Ping Pong/Multi-Head sur synthetic_fixture.

Ce programme est un oracle de test à réglages fixes, pas un moteur de production.
Il appelle le renderer C++ réel et ne qualifie aucune fidélité sonore Line 6.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import numpy as np

from echo_tools import read_wav, save_json, write_wav


ROOT = Path(__file__).resolve().parents[2]
FS = 48000
DELAY = 12000
FEEDBACK = 0.5
TONE_HZ = 4500.0
AMPLITUDE = 0.5
TAIL_SECONDS = 2
NUMERICAL_TOLERANCE = 2e-7


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pingpong_impulse_oracle(frames: int) -> np.ndarray:
    """Closed-form echo train for L=.5, R=0, wet=1; no delay engine."""
    expected = np.zeros((frames, 2), dtype=np.float64)
    for echo in range(1, (frames - 1) // DELAY + 1):
        channel = 0 if echo % 2 else 1
        expected[echo * DELAY, channel] = AMPLITUDE * 0.5 * FEEDBACK ** (echo - 1)
    return expected


def multihead_impulse_oracle(excitation: np.ndarray, frames: int) -> np.ndarray:
    """Independent float64 scalar difference equation, fixed integer taps.

    q[n] = (1-p)*(x[n]+g*q[n-D]) + p*q[n-1]
    y[n] = (q[n-D/3]+q[n-2D/3]+q[n-D])/3

    This analytical fixture oracle intentionally has no model selection, control
    smoothing, fractional delay, saturation, state serialization or audio API.
    """
    pole = math.exp(-2 * math.pi * TONE_HZ / FS)
    q = np.zeros((frames, 2), dtype=np.float64)
    for channel in range(2):
        previous = 0.0
        for n in range(frames):
            dry = float(excitation[n, channel]) if n < len(excitation) else 0.0
            longest = q[n - DELAY, channel] if n >= DELAY else 0.0
            previous = (1 - pole) * (dry + FEEDBACK * longest) + pole * previous
            q[n, channel] = previous
    expected = np.zeros((frames, 2), dtype=np.float64)
    for tap in (DELAY // 3, 2 * DELAY // 3, DELAY):
        expected[tap:] += q[:-tap] / 3
    return expected


def render(renderer: Path, fixture: Path, output: Path, model: str, block: int) -> dict:
    command = [str(renderer), "--input", str(fixture), "--output", str(output),
               "--model", model, "--delay-ms", "250", "--feedback", "0.5",
               "--mix", "1", "--tone-hz", "4500", "--drive", "0",
               "--mod-rate-hz", "0.7", "--mod-depth-ms", "2", "--tap-pattern", "0",
               "--feedback-lowpass-hz", "0", "--drift-amount", "0",
               "--feedback-highpass-hz", "0", "--lores-rate-envelope-amount", "0",
               "--digital-rack-rate-hz", "0",
               "--limiter", "0", "--sample-rate-hz", "0",
               "--tail", str(TAIL_SECONDS), "--block", str(block)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f"Renderer {model} en échec ({result.returncode}) : {result.stderr.strip()}")
    manifest = Path(str(output) + ".json")
    metadata = json.loads(manifest.read_text(encoding="utf-8"))
    expected = {"model": model, "delay_ms": 250, "feedback": FEEDBACK,
                "mix": 1, "tone_hz": TONE_HZ, "drive": 0}
    if metadata.get("parameters") != expected or metadata.get("normalized") is not False:
        raise ValueError(f"Paramètres effectifs inattendus : {model}")
    controls = metadata.get("controls", {})
    if (controls.get("limiter_enabled") is not False or controls.get("sample_rate_hz") != 0
            or controls.get("tap_pattern") != 0 or controls.get("feedback_lowpass_hz") != 0
            or controls.get("drift_amount") != 0 or controls.get("feedback_highpass_hz") != 0
            or controls.get("lores_rate_envelope_amount") != 0 or controls.get("digital_rack_rate_hz") != 0):
        raise ValueError(f"Contrôles analytiques non neutres : {model}")
    if metadata.get("input_sha256") != sha256(fixture) or metadata.get("output_sha256") != sha256(output):
        raise ValueError(f"Hash de manifeste différent : {model}")
    return {"command": command, "renderer_manifest": str(manifest.relative_to(ROOT)),
            "renderer_manifest_sha256": sha256(manifest), "build_id": metadata.get("build_id"),
            "source_sha256": metadata.get("source_sha256"), "input_class": "synthetic_fixture",
            "output_sha256": sha256(output)}


def compare(output: Path, expected: np.ndarray) -> dict:
    actual, fs, fmt = read_wav(output)
    if fs != FS or actual.shape != expected.shape:
        raise ValueError(f"Format ou dimensions inattendus : {output.name}")
    finite = bool(np.all(np.isfinite(actual)))
    if not finite:
        raise ValueError(f"Valeur non finie : {output.name}")
    error = actual - expected
    maximum = float(np.max(np.abs(error)))
    return {"file": str(output.relative_to(ROOT)), "frames": len(actual), "sample_rate_hz": fs,
            "format": fmt, "all_finite": finite, "max_error_absolute": maximum,
            "rms_error_absolute": np.sqrt(np.mean(error ** 2, axis=0)).tolist(),
            "peak_absolute": np.max(np.abs(actual), axis=0).tolist(),
            "passes_numeric_tolerance": maximum <= NUMERICAL_TOLERANCE}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, default=ROOT / "build/host/Release/echolabnt_render.exe")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/synthetic_fixture/new_models")
    args = parser.parse_args(argv)
    try:
        renderer = args.renderer.resolve()
        destination = args.output_dir.resolve()
        allowed = (ROOT / "output/synthetic_fixture").resolve()
        if destination == allowed or allowed not in destination.parents:
            raise ValueError("Choisir un sous-dossier de output/synthetic_fixture pour protéger les originaux")
        if not renderer.is_file():
            raise ValueError(f"Renderer absent : {renderer}")
        destination.mkdir(parents=True, exist_ok=True)
        fixture = destination / "impulse_left.wav"
        x = np.zeros((DELAY, 2), dtype=np.float64)
        x[0, 0] = AMPLITUDE
        write_wav(fixture, x, FS)
        fixture_metadata = {"schema_version": 1, "status": "synthetic_fixture", "input_class": "synthetic_fixture",
                            "file": str(fixture.relative_to(ROOT)), "sha256": sha256(fixture), "sample_rate_hz": FS,
                            "frames": len(x), "channels": ["L", "R"], "impulse": {"sample": 0, "L": AMPLITUDE, "R": 0},
                            "purpose": "Test de structure originale, aucune capture Line 6"}
        save_json(Path(str(fixture) + ".json"), fixture_metadata)
        frames = len(x) + TAIL_SECONDS * FS
        expected = {"pingpong": pingpong_impulse_oracle(frames),
                    "multihead": multihead_impulse_oracle(x, frames)}
        reports = {}
        for model in ("pingpong", "multihead"):
            renders = []
            for block in (64, 37):
                output = destination / f"{model}_block{block}.wav"
                build = render(renderer, fixture, output, model, block)
                check = compare(output, expected[model])
                check.update(build)
                renders.append(check)
            a, _, _ = read_wav(destination / f"{model}_block64.wav")
            b, _, _ = read_wav(destination / f"{model}_block37.wav")
            reports[model] = {"renders": renders, "block_64_vs_37_exact_match": bool(np.array_equal(a, b))}
        # Averages L and R into the wet feed: equal opposite impulses must cancel.
        anti = x.copy()
        anti[0, 1] = -AMPLITUDE
        anti_fixture = destination / "impulse_antiphase.wav"
        write_wav(anti_fixture, anti, FS)
        save_json(Path(str(anti_fixture) + ".json"), {**fixture_metadata, "file": str(anti_fixture.relative_to(ROOT)),
                  "sha256": sha256(anti_fixture), "impulse": {"sample": 0, "L": AMPLITUDE, "R": -AMPLITUDE}})
        anti_output = destination / "pingpong_antiphase.wav"
        anti_build = render(renderer, anti_fixture, anti_output, "pingpong", 64)
        anti_check = compare(anti_output, np.zeros((frames, 2)))
        anti_check.update(anti_build)
        reports["pingpong_antiphase"] = anti_check
        passed = all(r["block_64_vs_37_exact_match"] and all(q["passes_numeric_tolerance"] for q in r["renders"])
                     for key, r in reports.items() if key != "pingpong_antiphase")
        passed = passed and anti_check["max_error_absolute"] == 0
        report = {"schema_version": 1, "status": "synthetic_fixture", "input_class": "synthetic_fixture",
                  "created_utc": datetime.now(timezone.utc).isoformat(), "tool": "tools/analysis/verify_new_models.py",
                  "tool_sha256": sha256(Path(__file__)), "renderer": str(renderer), "renderer_sha256": sha256(renderer),
                  "parents": [{"path": str(fixture.relative_to(ROOT)), "sha256": sha256(fixture)},
                              {"path": str(anti_fixture.relative_to(ROOT)), "sha256": sha256(anti_fixture)}],
                  "configuration": {"sample_rate_hz": FS, "delay_samples": DELAY, "feedback": FEEDBACK,
                                    "mix": 1, "tone_hz": TONE_HZ, "drive": 0, "mod_rate_hz": 0.7, "mod_depth_ms": 2,
                                    "tap_pattern": 0, "limiter_enabled": False, "lores_sample_rate_hz": 0,
                                    "feedback_lowpass_hz": 0, "drift_amount": 0,
                                    "feedback_highpass_hz": 0, "lores_rate_envelope_amount": 0,
                                    "digital_rack_rate_hz": 0,
                                    "permanent_guard_ceiling": 0.891250938,
                                    "blocks": [64, 37], "tail_seconds": TAIL_SECONDS},
                  "numerical_absolute_tolerance": NUMERICAL_TOLERANCE,
                  "tolerance_scope": "Erreur d'arrondi float32 vs oracle float64, aucun seuil de fidélité produit",
                  "resampling": None, "gain_normalization": None, "removed_delay_samples": 0,
                  "oracles": {"pingpong": "train fermé : L@T=.25,R@2T=.125 puis alternance, gain .5 par répétition",
                              "multihead": "q[n]=(1-p)*(x[n]+.5*q[n-D])+p*q[n-1] ; trois taps D/3,2D/3,D divisés par3 ; p=exp(-2*pi*4500/Fs)"},
                  "checks": reports, "passed": passed, "fidelity": "not_qualified", "nt_validation": "not_run",
                  "qualification": "Vérification de structure originale à réglages fixes sur le renderer C++ réel ; aucune mesure Line 6."}
        report_path = destination / "verification.json"
        save_json(report_path, report)
        print(json.dumps({"passed": passed, "status": "synthetic_fixture", "report": str(report_path)}, ensure_ascii=False))
        return 0 if passed else 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Vérification impossible : {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
