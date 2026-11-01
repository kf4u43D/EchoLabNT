#!/usr/bin/env python3
"""Render local preset-bank auditions and measure signals; no listening judgement."""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import zipfile

import numpy as np

from echo_tools import read_wav, write_wav

ROOT = Path(__file__).resolve().parents[2]
CEILING = 10 ** (-1 / 20)
THRESHOLD = 10 ** (-60 / 20)
HIGH_DRIVE = {"analog_bounce": "analog", "tape_space": "tape", "tube_slap": "tube",
              "platter": "platter"}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def db(value: float) -> float | None:
    return 20 * math.log10(value) if value > 0 else None


def save_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def profile_values(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            key, separator, value = line.partition("=")
            if not separator:
                raise ValueError(f"Invalid profile row in {path}: {line}")
            values[key.strip()] = value.strip()
    return values


def describe_source(path: Path, kind: str, description: str) -> dict:
    samples, fs, encoding = read_wav(path)
    if not len(samples) or samples.shape[1] not in (1, 2) or not np.isfinite(samples).all():
        raise ValueError(f"Input must be a nonempty finite mono/stereo WAV: {path}")
    if not 8000 <= fs <= 384000:
        raise ValueError(f"Unsupported input sample rate: {fs}")
    active = np.flatnonzero(np.any(np.abs(samples) > THRESHOLD, axis=1))
    return {"path": str(path), "sha256": digest(path), "input_class": kind,
            "description": description, "frames": len(samples), "sample_rate_hz": fs,
            "duration_seconds": len(samples) / fs, "encoding": encoding,
            "last_input_sample_above_minus60_seconds": (int(active[-1]) + 1) / fs if len(active) else None}


def synthetic_sources(directory: Path, fs: int) -> dict[str, dict]:
    directory.mkdir(parents=True, exist_ok=True)
    frames = round(1.25 * fs)
    start, length = round(.1 * fs), fs
    t = np.arange(length, dtype=np.float64) / fs
    envelope = np.ones(length)
    attack, release = max(1, round(.02 * fs)), max(1, round(.08 * fs))
    envelope[:attack] = np.sin(np.linspace(0, math.pi / 2, attack)) ** 2
    envelope[-release:] = np.cos(np.linspace(0, math.pi / 2, release)) ** 2
    bass = .05 * np.sin(2 * math.pi * 55 * t) * envelope
    notes = (220.0, 277.1826309769, 329.6275569129)
    chord_l = sum(.035 * np.sin(2 * math.pi * frequency * t) for frequency in notes) * envelope
    chord_r = sum(.035 * np.sin(2 * math.pi * frequency * t + .15 * i) for i, frequency in enumerate(notes)) * envelope
    rng = np.random.default_rng(20261025)
    percussion = np.zeros(length)
    for onset in (0, .25, .5, .75):
        count = min(round(.15 * fs), length - round(onset * fs))
        local = np.arange(count, dtype=np.float64) / fs
        hit = (.07 * np.sin(2 * math.pi * 110 * local) * np.exp(-local / .025)
               + .025 * rng.standard_normal(count) * np.exp(-local / .008))
        offset = round(onset * fs)
        percussion[offset:offset + count] += hit
    payloads = {"bass": (np.column_stack((bass, bass)), "One-second 55 Hz bass burst, peak 0.05 before PCM24 quantization."),
                "chord": (np.column_stack((chord_l, chord_r)), "One-second sustained A-major three-note chord with 20 ms attack/80 ms release; no gain normalization."),
                "percussive": (np.column_stack((percussion, percussion)), "Four deterministic 110 Hz/noise hits; NumPy seed 20261025, no gain normalization.")}
    sources = {}
    for name, (active, description) in payloads.items():
        samples = np.zeros((frames, 2))
        samples[start:start + length] = active
        path = directory / f"synthetic_fixture_{name}.wav"
        write_wav(path, samples, fs)
        sources[name] = describe_source(path, "synthetic_fixture", description + " 100 ms leading/150 ms trailing silence.")
    return sources


def measure(samples: np.ndarray, fs: int, input_frames: int) -> tuple[dict, list[dict]]:
    if not len(samples):
        raise ValueError("Renderer produced an empty WAV")
    finite = np.isfinite(samples)
    nonfinite = int(samples.size - np.count_nonzero(finite))
    safe = np.where(finite, samples, 0.0)
    absolute = np.abs(safe)
    peak = float(np.max(absolute, initial=0))
    last_second = safe[-min(fs, len(safe)):]
    end_rms = float(np.sqrt(np.mean(last_second ** 2)))
    active = np.flatnonzero(np.any(absolute > THRESHOLD, axis=1))
    last_active = (int(active[-1]) + 1) / fs if len(active) else None
    window_frames = max(1, round(.1 * fs))
    windows = []
    for start in range(0, len(safe), window_frames):
        stop = min(start + window_frames, len(safe))
        rms = float(np.sqrt(np.mean(safe[start:stop] ** 2)))
        windows.append({"start_seconds": start / fs, "end_seconds": stop / fs,
                        "rms": rms, "rms_dbfs": db(rms)})
    return {"frames": len(samples), "duration_seconds": len(samples) / fs,
            "nonfinite_samples": nonfinite, "all_finite": nonfinite == 0,
            "sample_peak": peak, "sample_peak_dbfs": db(peak),
            "samples_above_minus1dbfs_ceiling": int(np.count_nonzero(absolute > CEILING + 1e-6)),
            "last_second_rms": end_rms, "last_second_rms_dbfs": db(end_rms),
            "last_time_above_minus60dbfs_seconds": last_active,
            "tail_to_last_sample_above_minus60_seconds": max(0.0, last_active - input_frames / fs) if last_active is not None else None,
            "end_above_minus60dbfs_rms": end_rms > THRESHOLD}, windows


def write_csv(path: Path, rows: list[dict], columns: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def playlist(path: Path, records: list[dict]) -> None:
    lines = ["#EXTM3U"]
    for record in records:
        lines.extend((f"#EXTINF:{record['duration_seconds']:.3f},{record['label']}", record["wav"]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, default=ROOT / "build/host/Release/echolabnt_render.exe")
    parser.add_argument("--input", type=Path, default=ROOT / "audio/but it sounds great.wav")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/audio/echolabnt_bank_2026-10-25")
    parser.add_argument("--tail", type=float, default=10, help="Seconds after the input; loops receive at least 8.5 seconds for their Stop event.")
    parser.add_argument("--profile-dir", type=Path, default=ROOT / "models/presets")
    parser.add_argument("--archive-dir", type=Path, default=ROOT / ".local/preset_archive/2026-10-25-before-musical-bank")
    parser.add_argument("--zip", action="store_true", help="Create a local listening ZIP containing rendered outputs/reports; excludes input WAVs.")
    args = parser.parse_args()
    try:
        renderer, source, out = args.renderer.resolve(), args.input.resolve(), args.output_dir.resolve()
        allowed = (ROOT / "output/audio").resolve()
        if allowed not in out.parents:
            raise ValueError("Output must be a subdirectory of the ignored output/audio directory")
        if out == source or out in source.parents:
            raise ValueError("Keep the original input outside the generated output directory")
        if not math.isfinite(args.tail) or not 0 < args.tail <= 3600:
            raise ValueError("--tail must be finite and between 0 and 3600 seconds")
        if not renderer.is_file():
            raise ValueError(f"Build the renderer first: {renderer}")
        profiles = sorted(args.profile_dir.resolve().glob("*.profile"))
        if not profiles:
            raise ValueError("No bank profiles found")
        out.mkdir(parents=True, exist_ok=True)
        sources = {"user": describe_source(source, "user_supplied_audio_unqualified", "Original user phrase; preserved in place and excluded from listening ZIP.")}
        sources.update(synthetic_sources(out / "stimuli", sources["user"]["sample_rate_hz"]))
        profile_hashes = {str(path): digest(path) for path in profiles}
        jobs = []
        for profile in profiles:
            for source_id, role in (("user", "bank_mix"), ("user", "wet"), ("bass", "wet"), ("chord", "wet"), ("percussive", "wet")):
                jobs.append((profile.stem, profile, source_id, role, None))
        archived_missing = []
        for preset_id, old_name in HIGH_DRIVE.items():
            profile = args.profile_dir.resolve() / f"{preset_id}.profile"
            if not profile.is_file():
                continue
            archive = args.archive_dir.resolve() / f"{old_name}.profile"
            for source_id in sources:
                jobs.append((preset_id, profile, source_id, "feedback28", .28))
                if archive.is_file():
                    jobs.append((preset_id, archive, source_id, "archived", None))
            if archive.is_file():
                profile_hashes[str(archive)] = digest(archive)
            else:
                archived_missing.append(str(archive))
        records, failed, window_rows = [], [], []
        for number, (preset_id, profile, source_id, role, feedback) in enumerate(jobs, 1):
            description = sources[source_id]
            fs, input_frames = description["sample_rate_hz"], description["frames"]
            settings = profile_values(profile)
            is_loop = settings.get("model") == "looper"
            tail = max(args.tail, 8.5) if is_loop else args.tail
            relative = Path(f"{source_id}_{role}") / f"{preset_id}.wav"
            target = out / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            command = [str(renderer), "--input", description["path"], "--output", str(target),
                       "--profile", str(profile), "--max-delay-ms", "8000", "--tail", str(tail)]
            if role != "bank_mix":
                command.extend(("--mix", "1"))
            if feedback is not None:
                command.extend(("--feedback", str(feedback)))
            stop_sample = None
            if is_loop:
                stop_sample = input_frames + 8 * fs
                events = target.with_suffix(".events.csv")
                events.write_text(f"sample,param,value\n{stop_sample},loop_action,stop\n", encoding="ascii")
                command.extend(("--events", str(events)))
            key = f"{preset_id}/{source_id}/{role}"
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=300)
                if result.returncode:
                    raise ValueError(result.stderr.strip() or result.stdout.strip())
                samples, actual_fs, encoding = read_wav(target)
                metadata_path = Path(str(target) + ".json")
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                metrics, windows = measure(samples, actual_fs, input_frames)
                valid = (actual_fs == fs and samples.shape == (input_frames + round(tail * fs), 2)
                         and metadata["input_sha256"] == description["sha256"]
                         and metadata["output_sha256"] == digest(target)
                         and metadata["profile_sha256"] == profile_hashes[str(profile)]
                         and not metadata["normalized"] and metrics["all_finite"]
                         and metrics["samples_above_minus1dbfs_ceiling"] == 0)
                record = {"key": key, "preset": preset_id, "source": source_id, "role": role,
                          "label": f"{preset_id} | {source_id} | {role}", "wav": relative.as_posix(),
                          "manifest": metadata_path.relative_to(out).as_posix(), "sha256": digest(target),
                          "input_class": description["input_class"], "profile": str(profile),
                          "profile_sha256": profile_hashes[str(profile)], "sample_rate_hz": actual_fs,
                          "encoding": encoding, "input_frames": input_frames, "tail_seconds": tail,
                          "loop_stop_sample": stop_sample, "decay_expected": not is_loop,
                          "build_id": metadata["build_id"], "parameters": metadata["parameters"],
                          "controls": metadata["controls"], "rhythm": metadata.get("rhythm"),
                          "command": command, "render_validation_passed": valid, **metrics}
                record["tail_review_required"] = not is_loop and record["end_above_minus60dbfs_rms"]
                records.append(record)
                window_rows.extend({"key": key, **window} for window in windows)
                if not valid:
                    failed.append({"key": key, "error": "Output format, provenance, finite signal or sample-ceiling check failed"})
                print(f"[{number}/{len(jobs)}] {key}: peak={metrics['sample_peak']:.6f}, last1sRMS={metrics['last_second_rms']:.7g}", flush=True)
            except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                failed.append({"key": key, "error": str(error), "command": command})
                print(f"[{number}/{len(jobs)}] FAILED {key}: {error}", file=sys.stderr, flush=True)
        lookup = {(item["preset"], item["source"], item["role"]): item for item in records}
        comparisons, ab_records = [], []
        for preset_id in HIGH_DRIVE:
            for source_id in sources:
                current = lookup.get((preset_id, source_id, "wet"))
                if not current:
                    continue
                for role in ("feedback28", "archived"):
                    reference = lookup.get((preset_id, source_id, role))
                    if reference:
                        comparisons.append({"preset": preset_id, "source": source_id, "new": current["key"], "reference": reference["key"],
                                            "scope": "feedback_only_28_percent" if role == "feedback28" else "whole_archived_profile_including_its_timing_and_other_settings",
                                            "new_feedback": current["parameters"]["feedback"], "reference_feedback": reference["parameters"]["feedback"],
                                            "new_last_second_rms_dbfs": current["last_second_rms_dbfs"], "reference_last_second_rms_dbfs": reference["last_second_rms_dbfs"],
                                            "new_tail_seconds_above_minus60": current["tail_to_last_sample_above_minus60_seconds"],
                                            "reference_tail_seconds_above_minus60": reference["tail_to_last_sample_above_minus60_seconds"]})
                        ab_records.extend((current, reference))
        sources_unchanged = all(digest(Path(item["path"])) == item["sha256"] for item in sources.values())
        profiles_unchanged = all(digest(Path(path)) == sha for path, sha in profile_hashes.items())
        report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
                  "status": "measured", "listening_status": "not_performed_by_this_tool", "sonic_quality": "not_assessed",
                  "hardware_validation": "not_run", "sonic_fidelity": "not_qualified", "local_auditions_only": True,
                  "renderer": str(renderer), "renderer_sha256": digest(renderer), "tool_sha256": digest(Path(__file__)),
                  "profile_count": len(profiles), "requested_renders": len(jobs), "completed_renders": len(records),
                  "sources": sources, "sources_unchanged": sources_unchanged, "profiles_unchanged": profiles_unchanged,
                  "gain_normalization": None, "rms_window_seconds": .1,
                  "metric_scope": "Last time above -60 dBFS is an absolute per-sample threshold, measured from input-file end for tail length. It depends on input level and is not a judgement of musical decay or quality. Last-second RMS averages both channels. Peaks are sample peaks, not true peaks.",
                  "loop_policy": "Preserve profile capture duration; Stop at input-file end + 8 seconds, then render at least 0.5 seconds more. Loops are not expected to decay naturally.",
                  "archived_comparisons_missing": archived_missing, "failed_renders": failed,
                  "tail_review_required": [item["key"] for item in records if item["tail_review_required"]],
                  "validation_passed": not failed and sources_unchanged and profiles_unchanged,
                  "comparisons": comparisons, "renders": records}
        save_json(out / "audition_report.json", report)
        columns = ["key", "preset", "source", "role", "wav", "sample_rate_hz", "duration_seconds", "sample_peak", "sample_peak_dbfs",
                   "last_second_rms", "last_second_rms_dbfs", "last_time_above_minus60dbfs_seconds", "tail_to_last_sample_above_minus60_seconds",
                   "nonfinite_samples", "samples_above_minus1dbfs_ceiling", "decay_expected", "tail_review_required", "render_validation_passed"]
        write_csv(out / "audition_metrics.csv", records, columns)
        write_csv(out / "rms_windows.csv", window_rows, ["key", "start_seconds", "end_seconds", "rms", "rms_dbfs"])
        playlist(out / "all_auditions.m3u", records)
        playlist(out / "user_bank_mix.m3u", [item for item in records if item["role"] == "bank_mix"])
        playlist(out / "wet_probes.m3u", [item for item in records if item["role"] == "wet"])
        playlist(out / "high_drive_ab.m3u", ab_records)
        if args.zip:
            with zipfile.ZipFile(out / "listening.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                names = {item[field] for item in records for field in ("wav", "manifest")}
                names.update(("audition_report.json", "audition_metrics.csv", "rms_windows.csv", "all_auditions.m3u", "user_bank_mix.m3u", "wet_probes.m3u", "high_drive_ab.m3u"))
                for name in sorted(names):
                    archive.write(out / name, name)
        print(json.dumps({"output": str(out), "renders": len(records), "failed": len(failed),
                          "tail_alerts": len(report["tail_review_required"]), "validation_passed": report["validation_passed"],
                          "listening_status": report["listening_status"]}), flush=True)
        return 0 if report["validation_passed"] else 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Audition bank failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
