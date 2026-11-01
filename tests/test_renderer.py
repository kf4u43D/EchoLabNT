"""Synthetic end-to-end renderer contract; never a reference fidelity test."""
import json
import hashlib
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


RENDERER = Path(sys.argv.pop(1)).resolve()
CEILING = struct.unpack("<f", struct.pack("<f", 10**(-1/20)))[0]


def write_wave(path, samples, *, bits=32, channels=1, floating=True, fs=8000):
    if floating:
        data = struct.pack("<" + "f" * len(samples), *samples)
    else:
        data = b"".join(int(sample).to_bytes(bits // 8, "little", signed=bits != 8) for sample in samples)
    fmt = struct.pack("<HHIIHH", 3 if floating else 1, channels, fs,
                      fs * channels * bits // 8, channels * bits // 8, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def read_float(path):
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise AssertionError("not RIFF/WAVE")
    position = 12
    while position < len(raw):
        tag, size = struct.unpack_from("<4sI", raw, position)
        if tag == b"data":
            return struct.unpack("<" + "f" * (size // 4), raw[position+8:position+8+size])
        position += 8 + size + (size & 1)
    raise AssertionError("missing data")


class RendererContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="echolab-renderer-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.input = self.root / "input.wav"
        self.output = self.root / "output.wav"

    def run_renderer(self, *options, success=True, neutral=True):
        options=list(map(str,options))
        if neutral:
            profile_keys=set()
            if "--profile" in options:
                profile_file=Path(options[options.index("--profile")+1])
                if profile_file.is_file():
                    profile_keys={line.split("=",1)[0].strip() for line in profile_file.read_text().splitlines() if "=" in line and not line.strip().startswith("#")}
            for flag,value in (("--limiter","0"),("--drive","0"),("--tone-hz","4500"),
                               ("--mod-rate-hz",".7"),("--mod-depth-ms","2"),("--sample-rate-hz","0"),
                               ("--feedback-lowpass-hz","0"),("--drift-amount","0"),
                               ("--feedback-highpass-hz","0"),("--lores-rate-envelope-amount","0"),
                               ("--digital-rack-rate-hz","0"),("--lores-cutoff-hz","0"),
                               ("--voicing","b06")):
                profile_has=flag[2:].replace("-","_") in profile_keys or (flag=="--limiter" and "limiter_enabled" in profile_keys)
                if flag not in options and not profile_has and not(flag=="--limiter" and "--limiter-enabled" in options):options.extend((flag,value))
        result = subprocess.run([str(RENDERER), "--input", str(self.input), "--output", str(self.output),
                                 *options], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_help(self):
        result = subprocess.run([str(RENDERER), "--help"], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("EchoLabNT offline renderer", result.stdout)
        self.assertIn("Usage: echolabnt_render", result.stdout)
        self.assertIn("--sync-mode", result.stdout)
        self.assertIn("--events", result.stdout)
        self.assertIn("pingpong", result.stdout)
        self.assertIn("multihead", result.stdout)
        self.assertIn("multihead|8bit|dynamic", result.stdout)
        self.assertIn("default .28", result.stdout)
        self.assertIn("default 0.39", result.stdout)

    def test_factory_feedback_and_mix_defaults(self):
        write_wave(self.input, [.5] + [0] * 15)
        models=("digital","analog","tube","pingpong","multihead","8bit","dynamic","tape","sweep",
                "analogmod","digitalmod","reverse","autovolume","digitalrack","platter","stereo","looper","exciter","sweepup","tubea04")
        for model in models:
            with self.subTest(model=model):
                self.run_renderer("--model",model,"--delay-ms",1,"--tail",.004,neutral=False)
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                self.assertAlmostEqual(metadata["parameters"]["feedback"],.28)
                self.assertAlmostEqual(metadata["parameters"]["mix"],.39)
        self.run_renderer("--model","digital","--delay-ms",1,"--tail",.004)
        output=read_float(self.output)
        for frame,gain in ((0,.5*(1-.39)),(8,.5*.39),(16,.5*.39*.28)):
            self.assertAlmostEqual(output[2*frame],gain,places=6)
            self.assertAlmostEqual(output[2*frame+1],gain,places=6)

    def test_pcm_formats_mono_duplication_and_no_normalization(self):
        for bits in (16, 24, 32):
            with self.subTest(bits=bits):
                scale = 1 << (bits-1)
                write_wave(self.input, [-scale//2, -scale//4, 0, scale//8], bits=bits, floating=False)
                self.run_renderer("--mix", 0, "--tail", 0)
                self.assertEqual(read_float(self.output), (-.5, -.5, -.25, -.25, 0, 0, .125, .125))
        write_wave(self.input, [.5, -.125, -.75, .25], channels=2)
        self.run_renderer("--mix", 0, "--tail", 0)
        self.assertEqual(read_float(self.output), (.5, -.125, -.75, .25))

    def test_impulse_feedback_tail_and_metadata(self):
        write_wave(self.input, [.5] + [0] * 15)
        self.run_renderer("--model", "digital", "--delay-ms", 1, "--feedback", .5,
                          "--mix", 1, "--tail", .004, "--diagnostic")
        output = read_float(self.output)
        self.assertEqual(len(output), 96)
        for frame in range(48):
            expected = .5 ** (frame//8) if frame and frame % 8 == 0 else 0
            self.assertEqual(output[frame*2], expected)
            self.assertEqual(output[frame*2+1], expected)
        metadata = json.loads(Path(str(self.output) + ".json").read_text())
        self.assertIs(metadata["normalized"], False)
        self.assertEqual(metadata["input_frames"], 16)
        self.assertEqual(metadata["tail_frames"], 32)
        self.assertEqual(metadata["sample_rate_hz"], 8000)
        self.assertEqual(metadata["fidelity"], "not_qualified")
        self.assertIn("build_id", metadata)
        self.assertEqual(metadata["input_sha256"], hashlib.sha256(self.input.read_bytes()).hexdigest())
        self.assertEqual(metadata["output_sha256"], hashlib.sha256(self.output.read_bytes()).hexdigest())

    def test_unsigned_pcm8_endpoints_silence_and_channel_order(self):
        raw = [0, 64, 128, 160, 255]
        write_wave(self.input, raw, bits=8, floating=False)
        self.run_renderer("--mix", 0, "--tail", 0)
        expected = [value for byte in raw for value in (max(-CEILING,min(CEILING,(byte-128)/128)),)*2]
        for actual, wanted in zip(read_float(self.output), expected): self.assertAlmostEqual(actual,wanted,places=6)
        raw = [0, 255, 128, 64, 160, 192, 129, 127, 128, 128]
        write_wave(self.input, raw, bits=8, floating=False, channels=2)
        self.run_renderer("--mix", 0, "--tail", 0)
        output=read_float(self.output)
        for frame in range(len(raw)//2):
            left,right=((byte-128)/128 for byte in raw[2*frame:2*frame+2])
            gain=min(1,CEILING/max(abs(left),abs(right))) if left or right else 1
            self.assertAlmostEqual(output[2*frame],left*gain,places=6)
            self.assertAlmostEqual(output[2*frame+1],right*gain,places=6)

    def test_unsigned_pcm8_direct_render_matches_exact_float_excitation(self):
        # All 256 byte codes, differing stereo content, and the requested musical
        # controls exercise the decoder through repeats rather than dry only.
        raw = [(i*37 + (i % 2)*83) % 256 for i in range(8192)]
        write_wave(self.input, raw, bits=8, floating=False, channels=2, fs=44100)
        options = ("--model", "digital", "--delay-ms", 250, "--feedback", .75,
                   "--mix", .5, "--tail", 1)
        self.run_renderer(*options)
        actual = self.output.read_bytes()
        metadata = json.loads(Path(str(self.output) + ".json").read_text())
        self.assertEqual(metadata["input_sha256"], hashlib.sha256(self.input.read_bytes()).hexdigest())
        self.assertEqual(metadata["sample_rate_hz"], 44100)
        self.assertEqual(metadata["input_channels"], 2)
        write_wave(self.input, [(byte-128)/128 for byte in raw], channels=2, fs=44100)
        self.run_renderer(*options)
        self.assertEqual(actual, self.output.read_bytes())

    def test_internal_tempo_default_and_musical_profile_overrides(self):
        write_wave(self.input,[.5]+[0]*15)
        self.run_renderer("--model","digital","--feedback",0,"--mix",1,"--tail",.6)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["rhythm"]["sync_mode"],"internal")
        self.assertEqual(metadata["rhythm"]["tempo_bpm"],120)
        self.assertEqual(metadata["rhythm"]["division"],"1/4")
        self.assertEqual(metadata["parameters"]["delay_ms"],500)
        self.assertEqual(read_float(self.output)[4000*2],.5)
        profile=self.root/"musical.profile"
        # No stored milliseconds: all supported notes resolve after CLI tempo.
        for note,quarters in (("1/1",4),("1/2",2),("1/4",1),("1/8",.5),("1/16",.25),
                              ("1/4D",1.5),("1/8D",.75),("1/4T",2/3),("1/8T",1/3),("1/16T",1/6)):
            with self.subTest(note=note):
                profile.write_text(f"model=digital\nsync_mode=internal\ntempo_bpm=90\ndivision={note}\nfeedback=0\nmix=1\n")
                self.run_renderer("--tempo-bpm",120,"--profile",profile,"--tail",2.1)
                musical_audio=self.output.read_bytes()
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                self.assertEqual(metadata["rhythm"]["division"],note)
                self.assertEqual(metadata["rhythm"]["tempo_bpm"],120)
                self.assertAlmostEqual(metadata["parameters"]["delay_ms"],500*quarters,places=4)
                self.run_renderer("--model","digital","--delay-ms",500*quarters,"--feedback",0,"--mix",1,"--tail",2.1)
                self.assertEqual(self.output.read_bytes(),musical_audio)
        profile.write_text("model=digital\nsync_mode=internal\ntempo_bpm=120\ndivision=1/8D\n")
        self.run_renderer("--profile",profile,"--delay-ms",5,"--tail",0)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["rhythm"]["sync_mode"],"free")
        self.assertEqual(metadata["parameters"]["delay_ms"],5)
        for options in (("--sync-mode","internal","--delay-ms",5),("--delay-ms",5,"--sync-mode","internal")):
            self.run_renderer("--profile",profile,*options,"--tail",0)
            metadata=json.loads(Path(str(self.output)+".json").read_text())
            self.assertEqual(metadata["parameters"]["delay_ms"],375)
            self.assertEqual(metadata["rhythm"]["sync_mode"],"internal")

    def test_stereo_note_divisions_and_independent_clipping(self):
        write_wave(self.input,[.5,-.25]+[0]*30,channels=2)
        profile=self.root/"stereo_notes.profile"
        for left,right,capacity,wanted_left,wanted_right in (("1/4","1/4D",2500,500,750),
                ("1/1","1/4",1000,1000,500),("1/4","1/1",1000,500,1000),
                ("1/16T","1/1",2500,500/6,2000)):
            with self.subTest(left=left,right=right):
                profile.write_text(f"model=stereo\nsync_mode=internal\ntempo_bpm=120\ndivision={left}\nright_division={right}\nfeedback=0\nmix=1\n")
                self.run_renderer("--profile",profile,"--max-delay-ms",capacity,"--tail",2.1)
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                rhythm=metadata["rhythm"]
                self.assertEqual(rhythm["division"],left)
                self.assertEqual(rhythm["right_division"],right)
                self.assertAlmostEqual(rhythm["effective_left_ms"],wanted_left,places=4)
                self.assertAlmostEqual(rhythm["effective_right_ms"],wanted_right,places=4)
                self.assertAlmostEqual(metadata["controls"]["stereo_ratio"],wanted_right/wanted_left,places=5)
                self.assertEqual(rhythm["left_clipped"],left=="1/1" and capacity==1000)
                self.assertEqual(rhythm["right_clipped"],right=="1/1" and capacity==1000)

    def test_musical_timing_events_and_optional_colour_profile_keys(self):
        write_wave(self.input,[.25,-.125]*128,channels=2)
        profile=self.root/"options.profile"
        profile.write_text("model=exciter\nsync_mode=internal\ntempo_bpm=120\ndivision=1/16\nexciter_highpass_enabled=1\nwet_tone_enabled=1\n")
        events=self.root/"notes.csv"
        events.write_text("sample,param,value\n0,division,1/8D\n17,tempo_bpm,150\n64,right_division,1/4T\n128,model,stereo\n200,sync_mode,free\n201,delay_ms,3\n")
        self.run_renderer("--profile",profile,"--events",events,"--tail",.1,"--block",17)
        expected=self.output.read_bytes()
        self.run_renderer("--profile",profile,"--events",events,"--tail",.1,"--block",64)
        self.assertEqual(self.output.read_bytes(),expected)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertTrue(metadata["controls"]["exciter_highpass_enabled"])
        self.assertTrue(metadata["controls"]["wet_tone_enabled"])
        self.assertEqual(metadata["event_count"],6)
        self.run_renderer("--profile",profile,"--exciter-highpass-enabled",0,"--wet-tone-enabled",0,"--tail",0)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertFalse(metadata["controls"]["exciter_highpass_enabled"])
        self.assertFalse(metadata["controls"]["wet_tone_enabled"])

    def test_coloured_stereo_presets_have_independent_first_arrivals(self):
        write_wave(self.input,[.125]+[0]*31)
        profiles=Path(__file__).resolve().parents[1]/"models/presets"
        for name,left_ms,right_ms in (("analog_stereo",375,500),("tube_stereo",500,1000/3),
                                      ("tape_stereo",250,375)):
            with self.subTest(preset=name):
                self.run_renderer("--profile",profiles/(name+".profile"),"--mix",1,"--feedback",0,
                                  "--modulation-enabled",0,"--limiter",0,"--tail",.75)
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                self.assertTrue(metadata["controls"]["stereo_timing_enabled"])
                self.assertAlmostEqual(metadata["rhythm"]["effective_left_ms"],left_ms,places=3)
                self.assertAlmostEqual(metadata["rhythm"]["effective_right_ms"],right_ms,places=3)
                audio=read_float(self.output)
                for channel,delay_ms in enumerate((left_ms,right_ms)):
                    start=int(delay_ms*8)
                    self.assertTrue(all(sample==0 for sample in audio[channel:2*(start-1):2]))
                    self.assertGreater(max(abs(sample) for sample in audio[2*start+channel:2*(start+16):2]),1e-7)

    def test_extended_route_names_and_timeline_block_independence(self):
        write_wave(self.input,[.125 if n%31<4 else 0 for n in range(1000)])
        events=self.root/"routes.csv"
        events.write_text("sample,param,value\n0,cv1,0.5\n127,cv1,-1\n500,cv1,1\n")
        for destination,number in (("mod_rate",9),("stereo_ratio",10),("drift",11),("duck_amount",12)):
            with self.subTest(destination=destination):
                expected=None
                for block in (1,24,511):
                    self.run_renderer("--model","dynamic" if number==12 else "tape","--delay-ms",5,
                        "--stereo-timing-enabled",1,"--mod1-source","cv1","--mod1-destination",destination,
                        "--mod1-amount",.2,"--events",events,"--tail",0,"--block",block)
                    actual=self.output.read_bytes()
                    if expected is None:expected=actual
                    else:self.assertEqual(actual,expected)
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                self.assertEqual(metadata["controls"]["mod1_destination"],number)

    def test_profile_override_and_events_block_invariance(self):
        write_wave(self.input, [((i*17) % 101 - 50) / 100 for i in range(1000)])
        profile = self.root / "control.profile"
        profile.write_text("# synthetic_fixture\nmodel=analog\ndelay_ms=2\nfeedback=0.7\nmix=0.8\ntone_hz=1200\ndrive=0.3\n")
        events = self.root / "events.csv"
        events.write_text("sample,param,value\n0,mix,0.7\n1,drive,0.9\n23,delay_ms,3\n24,model,tube\n25,feedback,0.4\n31,model,pingpong\n32,model,multihead\n64,tone_hz,3000\n64,mix,0.3\n65,model,digital\n999,mix,0.9\n1000,drive,0.1\n")
        expected = None
        for block in (1, 17, 24, 31, 64, 511, 4096):
            self.run_renderer("--profile", profile, "--mix", .2, "--events", events, "--block", block, "--tail", 0)
            audio = self.output.read_bytes()
            if expected is None:
                expected = audio
            self.assertEqual(audio, expected, f"block={block}")
        metadata = json.loads(Path(str(self.output) + ".json").read_text())
        self.assertAlmostEqual(metadata["parameters"]["mix"], .2)
        self.assertEqual(metadata["parameters"]["model"], "analog")
        self.assertEqual(metadata["event_count"], 12)
        self.assertEqual(metadata["profile"], str(profile))
        self.assertEqual(metadata["profile_sha256"], hashlib.sha256(profile.read_bytes()).hexdigest())
        self.assertEqual(metadata["events_sha256"], hashlib.sha256(events.read_bytes()).hexdigest())

    def test_new_model_profiles_cli_and_complete_parameter_metadata(self):
        write_wave(self.input, [1, -.5] + [0]*126, channels=2)
        profile = self.root / "heads.profile"
        events = self.root / "heads.csv"
        events.write_text("sample,param,value\n0,delay_ms,4\n1,feedback,0.25\n2,mix,0.9\n3,tone_hz,1500\n4,drive,0.7\n5,model,multihead\n25,model,pingpong\n")
        for model in ("pingpong", "multihead"):
            with self.subTest(model=model):
                profile.write_text(f"model={model}\ndelay_ms=3\nfeedback=0.6\nmix=0.7\ntone_hz=1000\ndrive=0.2\n")
                self.run_renderer("--profile", profile, "--events", events, "--tail", .1)
                metadata = json.loads(Path(str(self.output) + ".json").read_text())
                self.assertEqual(set(metadata["parameters"]), {"model", "delay_ms", "feedback", "mix", "tone_hz", "drive"})
                self.assertEqual(metadata["parameters"]["model"], model)
                for key, value in (("delay_ms", 3), ("feedback", .6), ("mix", .7), ("tone_hz", 1000), ("drive", .2)):
                    self.assertAlmostEqual(metadata["parameters"][key], value)
                self.assertEqual(metadata["event_count"], 7)
                self.assertEqual(metadata["fidelity"], "not_qualified")
                # The command line and key=value profile select the same engine.
                self.run_renderer("--model", model, "--delay-ms", 3, "--feedback", .6,
                                  "--mix", .7, "--tone-hz", 1000, "--drive", .2, "--tail", .1)
                cli_audio = self.output.read_bytes()
                self.run_renderer("--profile", profile, "--tail", .1)
                self.assertEqual(cli_audio, self.output.read_bytes())

    def test_pingpong_analytical_channel_alternation(self):
        write_wave(self.input, [.5] + [0]*63)
        self.run_renderer("--model", "pingpong", "--delay-ms", 1,
                          "--feedback", .5, "--mix", 1, "--tail", 0)
        actual = read_float(self.output)
        for frame in range(64):
            repeat = frame // 8
            gain = .5**repeat if frame and frame % 8 == 0 else 0
            self.assertEqual(actual[frame*2], gain if repeat % 2 else 0)
            self.assertEqual(actual[frame*2+1], gain if not repeat % 2 else 0)

    def test_bad_inputs_and_options_fail_readably(self):
        write_wave(self.input, [0] * 10)
        for options in (("--block", 0), ("--feedback", 2.1), ("--delay-ms", "nan"),
                        ("--model", "nonexistent"), ("--tail", -1), ("--unknown", 3),
                        ("--bit-depth", 8.5), ("--bit-depth", 25), ("--duck-amount", 1.1),
                        ("--mod-rate-hz", 0), ("--loop-action", "erase"),
                        ("--feedback-lowpass-hz", 10), ("--feedback-lowpass-hz", 20001), ("--drift-amount", 1.1),
                        ("--feedback-highpass-hz", 10), ("--feedback-highpass-hz", 2001),
                        ("--lores-rate-end-hz", 99), ("--lores-rate-decay-ms", 0), ("--lores-rate-envelope-amount", 1.1),
                        ("--digital-rack-rate-hz", 999), ("--digital-rack-rate-hz", 384001),
                        ("--lores-cutoff-hz", 19), ("--voicing", "a03"), ("--voicing", 4),
                        ("--modulation-enabled", 2), ("--tap-pattern", 11),
                        ("--sync-mode", "midi"), ("--tempo-bpm", 29), ("--tempo-bpm", 301),
                        ("--division", "1/7"), ("--right-division", "1/3"),
                        ("--exciter-highpass-enabled", 2), ("--wet-tone-enabled", -1),
                        ("--stereo-timing-enabled", 2), ("--mod1-destination", 13)):
            with self.subTest(options=options):
                result = self.run_renderer(*options, success=False)
                self.assertIn("echolabnt_render:", result.stderr)
        events = self.root / "invalid.csv"
        for text in ("sample,param,value\n2,mix,1\n1,mix,0\n", "sample,param,value\n100,mix,1\n"):
            events.write_text(text)
            self.run_renderer("--events", events, "--tail", 0, success=False)
        self.input.write_bytes(b"not a WAV")
        self.run_renderer(success=False)

    def test_sources_cannot_be_overwritten(self):
        write_wave(self.input, [1, 0])
        original = self.input.read_bytes()
        result = subprocess.run([str(RENDERER), "--input", str(self.input), "--output", str(self.input)],
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.input.read_bytes(), original)

        self.run_renderer("--metadata", self.input, success=False)
        self.assertEqual(self.input.read_bytes(), original)

    def test_complete_catalog_controls_and_aliases(self):
        write_wave(self.input, [0.25,-0.125]*64, channels=2)
        models=("digital","analog","tube","pingpong","multihead","8bit","dynamic","tape","sweep",
                "analogmod","digitalmod","reverse","autovolume","digitalrack","platter","stereo","looper","exciter","sweepup","tubea04")
        controls={"bit_depth":8,"mod_rate_hz":1.25,"mod_depth_ms":3,"duck_threshold_db":-30,
                  "duck_amount":.75,"attack_ms":2,"release_ms":100,"swell_ms":150,"stereo_ratio":1.75,"loop_action":1,
                  "limiter_enabled":0,"limiter_ceiling":.85,"limiter_release_ms":150,"sample_rate_hz":0,
                  "exciter_amount":.6,"exciter_tune_hz":3200,"tap_pattern":0,
                  "mod1_source":0,"mod1_destination":0,"mod1_amount":0,"mod2_source":0,"mod2_destination":0,"mod2_amount":0,
                  "feedback_lowpass_hz":0,"drift_amount":0,"feedback_highpass_hz":0,
                  "lores_rate_end_hz":1200,"lores_rate_decay_ms":1500,"lores_rate_envelope_amount":0,
                  "digital_rack_rate_hz":0,"lores_cutoff_hz":0,"voicing":0,"modulation_enabled":1,
                  "exciter_highpass_enabled":0,"wet_tone_enabled":0,"stereo_timing_enabled":0}
        options=[]
        for key,value in controls.items(): options.extend(("--"+key.replace("_","-"),value))
        for model in models:
            with self.subTest(model=model):
                self.run_renderer("--model",model,"--delay-ms",2,"--mix",.5,"--feedback",.75,"--tail",.01,*options)
                metadata=json.loads(Path(str(self.output)+".json").read_text())
                self.assertEqual(metadata["parameters"]["model"],model)
                self.assertEqual(set(metadata["controls"]),set(controls))
                for key,value in controls.items(): self.assertAlmostEqual(metadata["controls"][key],value,places=6)
        for alias,canonical in (("lores","8bit"),("ducking","dynamic")):
            self.run_renderer("--model",alias,"--tail",0)
            metadata=json.loads(Path(str(self.output)+".json").read_text())
            self.assertEqual(metadata["parameters"]["model"],canonical)
            aliased_audio=self.output.read_bytes()
            self.run_renderer("--model",canonical,"--tail",0)
            self.assertEqual(self.output.read_bytes(),aliased_audio)

    def test_extended_control_events_and_looper_block_invariance(self):
        write_wave(self.input,[.25,-.125,.1,-.2]*256,channels=2)
        events=self.root/"extended.csv"
        events.write_text("sample,param,value\n0,loop_action,record\n17,duck_amount,1\n65,bit_depth,6\n100,mod_rate_hz,2\n256,loop_action,play\n512,loop_action,overdub\n768,loop_action,stop\n900,loop_action,clear\n")
        for model in ("lores","dynamic","reverse","autovolume","looper"):
            self.run_renderer("--model",model,"--events",events,"--delay-ms",8,"--block",17,"--tail",.1)
            reference=self.output.read_bytes()
            self.run_renderer("--model",model,"--events",events,"--delay-ms",8,"--block",64,"--tail",.1)
            self.assertEqual(reference,self.output.read_bytes(),model)
    def test_duplicate_profiles_rejected_before_any_source_write(self):
        write_wave(self.input, [1, 0])
        first = self.root / "first.profile"
        second = self.root / "second.profile"
        first.write_text("model=analog\n")
        second.write_text("mix=0.5\n")
        original = first.read_bytes()
        result = subprocess.run([str(RENDERER), "--input", str(self.input), "--output", str(first),
                                 "--profile", str(first), "--profile", str(second)], text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Duplicate option: --profile", result.stderr)
        self.assertEqual(first.read_bytes(), original)

    def test_stronger_defaults_and_super_unity_feedback_are_limited(self):
        write_wave(self.input,[1.5,-.5]*128,channels=2)
        for model in ("digital","analog","tube","pingpong","multihead","lores","dynamic","tape","sweep",
                      "analogmod","digitalmod","reverse","autovolume","digitalrack","platter","stereo","looper","exciter","sweepup","tubea04"):
            self.run_renderer("--model",model,"--feedback",1.5,"--delay-ms",2,"--mix",.75,"--tail",.5,neutral=False)
            samples=read_float(self.output)
            self.assertTrue(all(abs(v)<=CEILING+1e-6 for v in samples),model)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertTrue(metadata["controls"]["limiter_enabled"])
        self.assertEqual(metadata["controls"]["sample_rate_hz"],8000)
        self.assertAlmostEqual(metadata["parameters"]["drive"],.4)
        self.assertAlmostEqual(metadata["controls"]["mod_depth_ms"],6)
        self.assertAlmostEqual(metadata["controls"]["mod_rate_hz"],1)
        self.assertAlmostEqual(metadata["controls"]["exciter_amount"],.1)
        self.assertAlmostEqual(metadata["controls"]["exciter_tune_hz"],min(4000,.4*metadata["sample_rate_hz"]))
        self.assertAlmostEqual(metadata["controls"]["drift_amount"],.25)
        self.assertAlmostEqual(metadata["controls"]["feedback_highpass_hz"],80)
        self.assertAlmostEqual(metadata["controls"]["lores_rate_envelope_amount"],1)
        self.assertAlmostEqual(metadata["controls"]["digital_rack_rate_hz"],min(16000,metadata["sample_rate_hz"]))
        self.assertAlmostEqual(metadata["controls"]["lores_cutoff_hz"],2000)
        self.assertEqual(metadata["controls"]["voicing"],0)
        self.assertTrue(metadata["controls"]["modulation_enabled"])

    def test_audio_rate_cv_routes_hashes_and_block_invariance(self):
        write_wave(self.input,[.4]+[0]*399)
        cv=self.root/"cv.wav";write_wave(cv,[1]*400)
        args=("--model","digital","--delay-ms",10,"--mix",1,"--tail",.1,
              "--mod1-source","cv1","--mod1-destination","time","--mod1-amount",1)
        self.run_renderer(*args)
        unmodulated=self.output.read_bytes()
        self.run_renderer(*args,"--cv1-input",cv,"--block",17)
        modulated=self.output.read_bytes()
        self.assertNotEqual(unmodulated,modulated)
        self.run_renderer(*args,"--cv1-input",cv,"--block",64)
        self.assertEqual(modulated,self.output.read_bytes())
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["cv1_sha256"],hashlib.sha256(cv.read_bytes()).hexdigest())
        original=cv.read_bytes()
        result=subprocess.run([str(RENDERER),"--input",str(self.input),"--output",str(cv),"--cv1-input",str(cv)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0);self.assertEqual(cv.read_bytes(),original)

    def test_multitap_patterns_are_exposed_and_distinct(self):
        write_wave(self.input,[.5]+[0]*511)
        renders=[]
        for pattern in range(11):
            self.run_renderer("--model","multihead","--tap-pattern",pattern,"--delay-ms",12,"--feedback",0,"--mix",1,"--tail",0)
            renders.append(self.output.read_bytes())
        self.assertEqual(len(set(renders)),11)

    def test_headroom_is_systematic_when_musical_stage_is_off(self):
        write_wave(self.input,[2,-.5,-4,1,.5,-.25],channels=2)
        self.run_renderer("--mix",0,"--limiter",0,"--limiter-ceiling",1,"--tail",0)
        actual=read_float(self.output)
        expected=(CEILING,-.25*CEILING,-CEILING,.25*CEILING,.5,-.25)
        for value,wanted in zip(actual,expected):self.assertAlmostEqual(value,wanted,places=6)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertAlmostEqual(metadata["controls"]["limiter_ceiling"],CEILING,places=6)

    def test_feedback_lowpass_placement_and_drift_block_invariance(self):
        write_wave(self.input,[.4]+[0]*511)
        base=("--model","digitalrack","--delay-ms",4,"--feedback",.75,"--mix",1,"--tail",.1)
        self.run_renderer(*base)
        plain=read_float(self.output)
        self.run_renderer(*base,"--feedback-lowpass-hz",1200)
        filtered=read_float(self.output)
        self.assertEqual(plain[:128],filtered[:128]) # First pass untouched before 2*Time.
        self.assertNotEqual(plain[128:],filtered[128:])
        self.run_renderer(*base,"--feedback-lowpass-hz",1200,"--drift-amount",.25,"--block",17)
        moving=self.output.read_bytes()
        self.run_renderer(*base,"--feedback-lowpass-hz",1200,"--drift-amount",.25,"--block",64)
        self.assertEqual(moving,self.output.read_bytes())
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["controls"]["feedback_lowpass_hz"],1200)
        self.assertEqual(metadata["controls"]["drift_amount"],.25)

    def test_feedback_highpass_preserves_first_pass(self):
        write_wave(self.input,[.4]+[0]*511)
        base=("--model","digitalrack","--delay-ms",4,"--feedback",.75,"--mix",1,"--tail",.1)
        self.run_renderer(*base)
        plain=read_float(self.output)
        self.run_renderer(*base,"--feedback-highpass-hz",200)
        filtered=read_float(self.output)
        self.assertEqual(plain[:128],filtered[:128])
        self.assertNotEqual(plain[128:],filtered[128:])

    def test_digital_rack_internal_rate_is_exposed(self):
        write_wave(self.input,[((n*37)%101-50)/125 for n in range(4096)],fs=44100)
        base=("--model","digitalrack","--delay-ms",5,"--mix",1,"--feedback",.75,"--tail",.1)
        self.run_renderer(*base)
        full=self.output.read_bytes()
        self.run_renderer(*base,"--digital-rack-rate-hz",16000,"--block",17)
        reduced=self.output.read_bytes()
        self.assertNotEqual(full,reduced)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["controls"]["digital_rack_rate_hz"],16000)
        self.run_renderer(*base,"--digital-rack-rate-hz",16000,"--block",64)
        self.assertEqual(reduced,self.output.read_bytes())

    def test_lores_rate_envelope_and_sweepup_are_exposed(self):
        write_wave(self.input,[((n*37)%101-50)/125 for n in range(4096)])
        base=("--model","lores","--sample-rate-hz",8000,"--delay-ms",5,"--mix",1,
              "--lores-rate-end-hz",1200,"--lores-rate-decay-ms",200,"--tail",.1)
        self.run_renderer(*base)
        fixed=self.output.read_bytes()
        self.run_renderer(*base,"--lores-rate-envelope-amount",1,"--block",17)
        falling=self.output.read_bytes()
        self.assertNotEqual(fixed,falling)
        self.run_renderer(*base,"--lores-rate-envelope-amount",1,"--block",64)
        self.assertEqual(falling,self.output.read_bytes())
        for model in ("sweep","sweepup"):
            self.run_renderer("--model",model,"--mix",1,"--delay-ms",5,"--mod-rate-hz",2,"--tail",.1)
            if model=="sweep":down=self.output.read_bytes()
            else:self.assertNotEqual(down,self.output.read_bytes())
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["parameters"]["model"],"sweepup")

    def test_selected_voicings_and_internal_modulation_switch(self):
        write_wave(self.input,[((n*37)%101-50)/125 for n in range(1024)])
        for model,wanted in (("analog","a04"),("analogmod","a04"),("exciter","a04"),
                             ("digital","a05"),("autovolume","a05"),("digitalrack","b06"),
                             ("lores","b06"),("tube","b06"),("tubea04","a04")):
            self.run_renderer("--model",model,"--voicing","auto","--tail",0)
            metadata=json.loads(Path(str(self.output)+".json").read_text())
            self.assertEqual(metadata["resolved_voicing"],wanted)
        args=("--model","analogmod","--voicing","a05","--delay-ms",5,"--mix",1,"--tail",.1,
              "--mod-rate-hz",1,"--mod-depth-ms",4)
        self.run_renderer(*args)
        moving=self.output.read_bytes()
        self.run_renderer(*args,"--modulation-enabled",0,"--block",17)
        still=self.output.read_bytes()
        self.assertNotEqual(moving,still)
        self.run_renderer(*args,"--modulation-enabled",0,"--block",64)
        self.assertEqual(still,self.output.read_bytes())

    def test_lores_cv_rate_route_survives_internal_modulators_off(self):
        write_wave(self.input,[((n*37)%101-50)/125 for n in range(4096)],fs=44100)
        cv=self.root/"cv_rate.wav"
        write_wave(cv,[-.75]*4096,fs=44100)
        args=("--model","lores","--sample-rate-hz",8000,"--delay-ms",5,"--mix",1,"--tail",.1,
              "--lores-rate-envelope-amount",0,"--modulation-enabled",0,
              "--mod1-source","cv1","--mod1-destination","fe","--mod1-amount",1)
        self.run_renderer(*args)
        plain=self.output.read_bytes()
        self.run_renderer(*args,"--cv1-input",cv,"--block",17)
        modulated=self.output.read_bytes()
        self.assertNotEqual(plain,modulated)
        self.run_renderer(*args,"--cv1-input",cv,"--block",64)
        self.assertEqual(modulated,self.output.read_bytes())
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["controls"]["mod1_destination"],8)
        self.assertEqual(metadata["controls"]["modulation_enabled"],False)

    def test_swell_factory_recipe_omits_dry_attack_and_rises(self):
        write_wave(self.input,[.4*math.sin(2*math.pi*220*n/8000) for n in range(32000)])
        profile=Path(__file__).resolve().parents[1]/"models/presets/swell.profile"
        self.run_renderer("--profile",profile,"--tail",0)
        metadata=json.loads(Path(str(self.output)+".json").read_text())
        self.assertEqual(metadata["parameters"]["model"],"autovolume")
        self.assertEqual(metadata["parameters"]["mix"],1)
        self.assertEqual(metadata["controls"]["swell_ms"],1200)
        self.assertFalse(metadata["controls"]["modulation_enabled"])
        self.assertEqual(metadata["rhythm"]["effective_left_ms"],500)
        wet=read_float(self.output)[::2]
        self.assertTrue(all(sample==0 for sample in wet[:4000]))
        early=sum(abs(sample) for sample in wet[4000:4800])/800
        late=sum(abs(sample) for sample in wet[20000:24000])/4000
        self.assertGreater(early,0)
        self.assertGreater(late,early*2)
        self.assertLessEqual(max(abs(sample) for sample in wet),CEILING+1e-6)

    def test_factory_envelopes_release_the_existing_stereo_tail(self):
        # Left stops first. The right input must keep the shared duck active;
        # afterward even a nonzero input below threshold must release it.
        fs=8000
        samples=[]
        for n in range(fs*2):
            samples.extend((.35*math.sin(2*math.pi*220*n/fs) if n<fs*.4 else 0,
                            (.7 if n<fs else .004)*math.sin(2*math.pi*440*n/fs)))
        write_wave(self.input,samples,channels=2,fs=fs)
        def rms(wet,start,end):
            window=wet[int(start*fs):int(end*fs)]
            return math.sqrt(sum(v*v for v in window)/len(window))
        for recipe in ("ducked_voice","bloom","swell"):
            with self.subTest(recipe=recipe):
                profile=Path(__file__).resolve().parents[1]/f"models/presets/{recipe}.profile"
                args=("--profile",profile,"--mix",1,"--tail",.5)
                self.run_renderer(*args,neutral=False)
                wet=read_float(self.output)[::2]
                self.run_renderer(*args,"--duck-amount",0,neutral=False)
                unducked=read_float(self.output)[::2]
                during=rms(wet,.7,.8)/rms(unducked,.7,.8)
                recovering=rms(wet,1.3,1.4)/rms(unducked,1.3,1.4)
                recovered=rms(wet,1.8,1.9)/rms(unducked,1.8,1.9)
                self.assertLess(during,.3)
                self.assertGreater(recovering,.45)
                self.assertGreater(recovered,.8)
                self.assertGreater(recovering,during*2)
                self.assertGreater(rms(unducked,1.3,1.4),1e-7)
                self.assertTrue(all(math.isfinite(v) and abs(v)<=CEILING+1e-6 for v in wet))

    def test_autovolume_can_produce_a_clear_wet_swell(self):
        write_wave(self.input,[.4]*8000)
        self.run_renderer("--model","autovolume","--delay-ms",5,"--mix",1,"--feedback",0,
                          "--swell-ms",600,"--attack-ms",1,"--duck-threshold-db",-40,"--tail",0)
        wet=read_float(self.output)[::2]
        early=sum(abs(v) for v in wet[40:840])/800
        late=sum(abs(v) for v in wet[6400:7200])/800
        self.assertGreater(late,early*4)
        self.assertLessEqual(max(abs(v) for v in wet),.4)


if __name__ == "__main__":
    unittest.main()
