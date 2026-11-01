# EchoLabNT installation

For Expert Sleepers disting NT, targeting firmware 1.18.0 / API 13.

## Install the plug-in

1. Extract the release archive.
2. Back up your presets and the existing plug-in file, if present.
3. With the module powered off, copy `echolabnt.o` to `/programs/plug-ins/` on its MicroSD card. Keep only one copy of this plug-in in that folder, replacing the previous file even if its filename differs.
4. Safely eject the card, return it to the module and power on.
5. Add **EchoLabNT** from the algorithm menu.

Older files named `coloured_echos.o` or `echo_lab_nt.o` use the same algorithm ID as `echolabnt.o`. Keep their backups outside the entire `/programs/plug-ins/` folder tree when installing the new version.

Check `BUILD.txt` in the archive for its version, build ID and plug-in checksum. `SHA256SUMS.txt` lists all file checksums. The `.profile` files are settings for the computer renderer, rather than presets that can be loaded directly on the module.

Updating the plug-in preserves settings in saved NT presets. To apply a factory recipe's revised defaults, select **Custom**, then choose that recipe again. **Platter** has internal modulation and drift off; an older saved setting can retain its previous motion until you reapply the recipe.

## Route audio

The default inputs are buses **1/2**, and the default outputs are **13/14**. Choose the input and output buses on the Routing page.

- **Replace** writes the processed signal to the output bus. Use this for an insert.
- **Add** mixes the processed signal with audio already on the output bus.
- **None** supplies silence at an input or disables an output.
- Sending both outputs to the same bus produces a mono average, using the left output mode.

Select **Max delay** when adding the algorithm: 1 to 8 seconds, with a default of 3 seconds. Time stays within that capacity.

For a mono source feeding a stereo delay, set **Input L** and **Input R** to the same source bus. Keep **Output L** and **Output R** on separate buses for stereo playback. Setting an input to None supplies silence to that channel; the Stereo model does not copy audio from the other input.

## Set the sound

Choose a musical **Preset** on the Model page, or select **Custom** and a Model. A new instance starts on **Analog Bounce**, with **Internal BPM at 120 BPM**, dotted eighth notes, Feedback 13%, Mix 39%, Tone 3000 Hz and Drive 42.8%. Saved NT presets retain their sound. Then adjust **Time**, **Feedback** and **Mix**. Model-specific pages expose controls such as Tone, Drive, Mod rate/depth, tap pattern, sample rate, ducking or swell.

**Sweep Down** uses a descending low-pass sweep; **Sweep Up** uses an ascending high-pass sweep. A new input phrase triggers each sweep, whose duration follows Mod rate.

Choose **Swell** for a slow, fully wet entrance with quarter-note echoes. Its 1200 ms rising envelope is adjustable on the Swell page. **Bloom** provides a quicker 600 ms envelope and shorter echoes. Both duck the wet return during the phrase and release its stored tail below Threshold. Adjust Duck amount for the depth and Release for the return speed; zero Duck amount leaves only the rising envelope. Detection uses both inputs. Keep Mix at 100% to avoid adding the original dry attack.

**Internal modulation** switches the internal LFO, wow, flutter and drift together. External CV and the MIDI modulation wheel remain available. Two internal mapping slots connect a modulation source to Time, Feedback, Tone, Drive, Mix, Depth, Exciter, 8bit sample rate, Mod rate, Stereo ratio, Drift or Duck amount. Choose CV buses on the CV inputs page; a ±5 V signal spans the modulation range.

Target selection skips controls that do not affect the current model. A saved mapping that is currently inactive shows **(off)** and retains its setting. CV sources also show (off) until their input bus is assigned; internal LFO and envelope sources need Internal modulation on. Selecting an LFO or envelope source exposes its rate or attack/release controls where needed. For the looper, Time sets the next recording length and Feedback controls overdub retention.

The first **Delay** page shows **Time** in milliseconds in Free mode. With Internal BPM or a locked MIDI/CV clock, its first control becomes **Division**, using straight, dotted or triplet note values. The Clock page selects the clock source and internal tempo; CV also needs a Clock bus and pulses per quarter note. External clock waiting, Stop or more than two seconds without pulses returns the first control to manual Time.

On **Color**, **Version A** keeps the model's original character; **Version B** selects the alternate character. Models with a single character omit this selector. Existing saved settings are retained until you change Version.

For **Multi-Head**, open **Heads** and set **Tap pattern**. A pattern places taps within the complete delay span selected on the Delay page. In sync, Heads also offers the span's Division and displays the tap positions as notes where possible. The taps are preset patterns, rather than individually editable notes. D means dotted and T means triplet; the Golden pattern uses fractions of the span.

For **Stereo Delays**, open **Stereo**. In sync, **Division** sets the left delay and **Right division** selects the right delay as a musical note. Choose **Ratio** to use the stored R/L delay ratio instead. Free mode and a missing external clock use that ratio. Each channel is limited independently by Max delay. The screen reports max delay if a requested note exceeds this capacity.

For **Analog**, **Analog Mod**, both **Tube** models and **Tape**, enable **Stereo timing** on the Stereo page to use these independent left/right rhythms with the selected colour. The Analog Stereo, Tube Stereo and Tape Stereo recipes enable it for you. Feed a mono source to both inputs to hear both sides.

## Map CV and MIDI controls

The NT's native **Mappings** menu controls the exposed algorithm parameters beyond the two internal mapping slots. Select EchoLabNT, then **CV Mappings**: choose the parameter, source and bus, and set Volts/Delta. It adds `CV × Delta / Volts` to the base value. Save the NT preset to retain mappings; renderer `.profile` files do not store them.

For MIDI, open **MIDI Mappings**, select the parameter and channel, enable the mapping and set Min/Max. Choose **CC** for 7-bit control. For 14-bit control, send a matched pair such as **CC16 + CC48** on one channel. Set **CC/note = 16** and select **14 bit CC - high first** if CC16 arrives first, or **low first** if CC48 arrives first. The pair supplies a value from 0 to 16383; the parameter retains its defined step size.

The internal **Mod wheel** source reads CC1 in 7 bits only. Use native MIDI mapping for 14-bit control. See the [official firmware 1.18 manual, pages 44–47](https://www.expert-sleepers.co.uk/downloads/manuals/disting_NT_user_manual_1.18.pdf#page=44).

## Bypass and looping

**Active** processes normally. **Dry** passes the input while existing echoes decay internally. **Tails** passes the input together with the remaining echoes. The output limiter remains active.

Loop Sampler provides **Record**, **Play**, **Overdub**, **Stop** and **Clear**. Time or Division sets the next recording length. Audio stays in memory only; saved presets do not contain the loop, and changing the model clears it.

The [preset guide](PRESETS.md) describes the 21 recipes, including Swell, Stereo Weave's 3:2 rhythm and the three coloured stereo rhythms. Preset changes retain active clock source, tempo, CV input buses and audio routing; selecting a recipe from Free enables Internal BPM. Applying a recipe resets its sound controls and internal modulation mappings; editing a recipe control selects Custom.

If the host is still applying a recipe's values, let the controls settle before making more manual model edits. A rapid-edit case remains reproducible with simulated delayed notifications; physical confirmation is pending.
