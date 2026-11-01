# EchoLabNT 0.7.1-rc.5

A stereo delay collection for Expert Sleepers disting NT, with 20 treatments, 21 musical presets, eleven multi-head patterns, reverse delay, ducking, auto-volume, a loop sampler, CV modulation and MIDI/CV clock synchronization.

Target: disting NT firmware 1.18.0 / API 13.

Preset changes made before the first audio block now apply their model and settings after initialization or UI exposure. New instances start on Analog Bounce; existing saved settings are preserved. Ducked Voice, Swell and Bloom release the echoes after the input drops below Threshold. Attack and Release control the ducking gain directly. Swell and Bloom retain the recorded phrase and apply their rising envelope to the wet return, so a short phrase can leave a tail. Their Swell page now includes Duck amount. `BUILD.txt` identifies the exact plug-in.

The bank also includes distinct Analog, Tube and Tape stereo rhythms, increased Drive on the character presets, a steady Platter recipe and shorter model names. Swell uses a slow, fully wet envelope with quarter-note repeats; Bloom has a quicker rise and shorter echoes. This candidate includes host regression tests and an ARM build; its fixes still require confirmation on the module.

A rapid-edit corner case remains reproducible with simulated delayed host notifications: an older queued value can replace a later manual selection. It is not confirmed on hardware. Let recipe controls settle before further manual model edits; this prerelease does not claim that all synchronization errors have been eliminated.

## Installation

Extract the archive and copy `echolabnt.o` to `/programs/plug-ins/` on the module's MicroSD card. Safely eject the card, return it to the powered-off module and restart. Add **EchoLabNT**, select a model, then adjust Time, Feedback and Mix.

Back up existing presets and plug-in files before replacing them. Keep a single copy of the algorithm in the plug-ins folder tree, replacing older files even if named `coloured_echos.o` or `echo_lab_nt.o`.

The archive includes [installation instructions](docs/INSTALL_NT.md), renderer profiles and file checksums.

The internal clock starts at 120 BPM. The [preset guide](docs/PRESETS.md) covers driven analog echoes, tape repeats, a simpler Platter preset, descending 8bit textures, swells, ducking and slapback. Analog Stereo, Tube Stereo and Tape Stereo add distinct left/right rhythms with their original colour engines. Four new internal modulation destinations complement the NT's native CV and 7/14-bit MIDI CC mappings.
