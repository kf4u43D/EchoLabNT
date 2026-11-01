# Building EchoLabNT on Windows

Use PowerShell 7, Git, CMake, Visual Studio 2022 C++ Build Tools and Python. ARM builds also need Arm GNU Toolchain tools on PATH; the tested version is 14.3.Rel1. Dependency versions and the pinned API13 SDK are recorded in [dependencies.lock.json](https://github.com/kf4u43D/EchoLabNT/blob/main/third_party/dependencies.lock.json).

Run these commands from the repository root:

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

The package is written to `dist/`; `BUILD.txt` identifies its exact object and checksum. These commands perform local builds and tests. The tests use synthetic fixtures; physical loading and performance remain separate checks. A rapid-edit case with simulated delayed parameter notifications remains documented in [release notes](../RELEASE.md).

Edit the factory bank in `models/preset_bank.json`, then run `.venv/Scripts/python.exe tools/build/generate_presets.py` to regenerate profiles and the NT table. Preserve published parameter, model and recipe IDs. See [third-party notices](https://github.com/kf4u43D/EchoLabNT/blob/main/THIRD_PARTY_NOTICES.txt) for licensing information.
