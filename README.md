# EchoLabNT

**Current release: [0.7.1-rc.5 (prerelease)](https://github.com/kf4u43D/EchoLabNT/releases/tag/v0.7.1-rc.5).**

[Download the disting NT plug-in](https://github.com/kf4u43D/EchoLabNT/releases/download/v0.7.1-rc.5/EchoLabNT-public-0.7.1-rc.5.zip) · [Release notes and known limitations](https://github.com/kf4u43D/EchoLabNT/releases/tag/v0.7.1-rc.5)

A stereo delay plug-in for Expert Sleepers disting NT, with 20 treatments and 21 musical presets covering analog, tube, tape, platter, digital, multi-head, swept filters, reverse, ducking, auto-volume and looping. Shape repeats with tone, drive, modulation, CV and MIDI clock, or use the internal tempo.

Target: disting NT firmware 1.18.0 / API 13.

## Install

1. Download and extract the release archive.
2. Back up your current presets and plug-in files.
3. With the module powered off, copy `echolabnt.o` to `/programs/plug-ins/` on its MicroSD card. Keep a single copy of this algorithm: replace older files even if named `coloured_echos.o` or `echo_lab_nt.o`, and store backups outside the plug-ins folder tree.
4. Safely eject the card, insert it into the module and restart.
5. Add **EchoLabNT**, then choose a model and set Time, Feedback and Mix.

See [installation and controls](docs/INSTALL_NT.md) for routing, clock and modulation settings.

New instances start on **Analog Bounce**, with Internal tempo at 120 BPM and dotted eighth-note repeats. Choose another musical recipe from **Model → Preset**; note divisions follow the selected clock. See the [preset guide](docs/PRESETS.md).

**Stereo Weave** plays a 3:2 rhythm across the two outputs. For a mono source, select the same input bus for both Input L and Input R; use separate left and right output buses to hear the stereo pattern.

**Analog Stereo**, **Tube Stereo** and **Tape Stereo** combine their own colour with independent left/right rhythms. The NT's native parameter mappings support CV and MIDI CC in 7 or 14 bits; see the installation guide for setup.

**Swell** softens entrances with a slow, fully wet envelope and quarter-note repeats. **Bloom** offers a quicker envelope with shorter echoes. Both keep the echoes subdued while you play and release the tail after the phrase; adjust Duck amount and Release on the Swell page.

## Build from source on Windows

Use PowerShell 7, Git, CMake, Visual Studio 2022 C++ Build Tools and Python. ARM builds also need Arm GNU Toolchain tools on PATH; the tested version is 14.3.Rel1. Dependency versions and the pinned API13 SDK are recorded in [dependencies.lock.json](https://github.com/kf4u43D/EchoLabNT/blob/main/third_party/dependencies.lock.json).

From a source checkout:

```powershell
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r tools/analysis/requirements.lock.txt
pwsh -NoProfile -File tools/import/fetch_sdk.ps1
$python = (Resolve-Path .venv/Scripts/python.exe).Path
cmake --preset windows "-DPython3_EXECUTABLE=$python"
cmake --build --preset windows
ctest --preset windows
pwsh -NoProfile -File tools/build/build_nt.ps1
pwsh -NoProfile -File tools/build/package.ps1
```

The package is written to `dist/`; `BUILD.txt` identifies its exact object and checksum. These commands perform local builds and tests. The tests use synthetic fixtures; physical loading and performance remain separate checks. A rapid-edit case with simulated delayed parameter notifications remains documented in [release notes](RELEASE.md).

Edit the factory bank in `models/preset_bank.json`, then run `.venv/Scripts/python.exe tools/build/generate_presets.py` to regenerate profiles and the NT table. Preserve published parameter, model and recipe IDs. See [third-party notices](https://github.com/kf4u43D/EchoLabNT/blob/main/THIRD_PARTY_NOTICES.txt) for licensing information.
