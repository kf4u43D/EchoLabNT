"""Factory recipe consistency and safe deterministic generation contracts."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('generate_presets',ROOT/'tools/build/generate_presets.py')
generator=importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class FactoryBank(unittest.TestCase):
    def test_nt_default_is_active_analog_without_changing_profile_values(self):
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        self.assertEqual(bank['default_preset'],'analog_bounce')
        initial=next(p for p in bank['presets'] if p['id']==bank['default_preset'])
        self.assertEqual(initial['slot'],4)
        self.assertNotIn(initial['slot'],bank['retired_slots'])
        self.assertEqual({**bank['defaults'],**initial['parameters']}['model'],'analog')
        generated=generator.bank_outputs()
        header=generated[ROOT/'src/nt/factory_presets.h']
        self.assertIn('inline constexpr unsigned defaultSlot=4;',header)
        self.assertIn('constexpr std::int16_t defaultValue(',header)
        with tempfile.TemporaryDirectory(prefix='echolab-default-') as temporary:
            root=Path(temporary)
            (root/'models').mkdir()
            path=root/'models/preset_bank.json'
            bank['default_preset']='tube_slap'
            path.write_text(json.dumps(bank),encoding='utf-8')
            alternate=generator.bank_outputs(root)
            profiles={p.name:text for p,text in generated.items() if p.suffix=='.profile'}
            other_profiles={p.name:text for p,text in alternate.items() if p.suffix=='.profile'}
            self.assertEqual(profiles,other_profiles)
            self.assertIn('inline constexpr unsigned defaultSlot=6;',alternate[root/'src/nt/factory_presets.h'])
            for invalid in (None,'missing','against_three',''):
                with self.subTest(default=invalid):
                    bank['default_preset']=invalid
                    path.write_text(json.dumps(bank),encoding='utf-8')
                    with self.assertRaisesRegex(ValueError,'active factory recipe'):
                        generator.bank_outputs(root)

    def test_generated_bank_is_current_and_stores_musical_time(self):
        outputs=generator.bank_outputs()
        self.assertEqual(generator.stale_outputs(outputs),[])
        profiles={p:text for p,text in outputs.items() if p.suffix=='.profile'}
        self.assertEqual(len(profiles),21)
        for path,text in profiles.items():
            with self.subTest(profile=path.name):
                fields=dict(line.split('=',1) for line in text.splitlines() if line and not line.startswith('#'))
                self.assertNotIn('delay_ms',fields)
                self.assertEqual(fields['sync_mode'],'internal')
                self.assertEqual(fields['tempo_bpm'],'120')
                self.assertIn(fields['division'],generator.NOTES)
                self.assertEqual(fields['mod1_source'],'0')
                self.assertEqual(fields['mod2_source'],'0')
                if fields['modulation_enabled']=='0':self.assertEqual(fields['drift_amount'],'0')

    def test_each_nt_recipe_resets_sound_and_preserves_routing_and_manual_time(self):
        source=(ROOT/'src/nt/echo_lab.cpp').read_text(encoding='utf-8')
        enum=re.search(r'enum Parameter\s*\{(.*?)ParameterCount',source,re.S).group(1)
        all_parameters=set(re.findall(r'\b[A-Z][A-Za-z0-9_]*\b',enum))
        preserved={'InputL','InputR','OutputL','OutputModeL','OutputR','OutputModeR','Time',
                   'CV1Bus','CV2Bus','ClockBus','FactoryPreset'}
        required=all_parameters-preserved
        header=(ROOT/'src/nt/factory_presets.h').read_text(encoding='utf-8')
        arrays=re.findall(r'inline constexpr Value p\d+\[\]=\{(.*?)\n\};',header,re.S)
        self.assertEqual(len(arrays),21)
        for index,array in enumerate(arrays):
            with self.subTest(recipe=index):
                pairs=re.findall(r'\{(\w+),(-?\d+)\}',array)
                self.assertEqual(len(pairs),len(dict(pairs)))
                self.assertEqual(set(dict(pairs)),required)
                self.assertTrue(all(-32768<=int(value)<=32767 for _,value in pairs))
                self.assertEqual(dict(pairs)['SyncMode'],'3')
                self.assertEqual(dict(pairs)['TempoBpm'],'120')
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        recipes={p['id']:{**bank['defaults'],**p['parameters']} for p in bank['presets']}
        self.assertEqual(recipes['tube_slap']['division'],'1/16')
        self.assertEqual(recipes['tube_slap']['feedback'],.10)
        self.assertEqual(recipes['bloom']['mix'],1)
        self.assertEqual(recipes['stereo_weave']['right_division'],'1/4T')
        self.assertEqual(recipes['stereo_weave']['division'],'1/4')
        self.assertEqual(recipes['stereo_weave']['model'],'stereo')
        self.assertEqual(recipes['stereo_weave']['feedback'],.32)
        self.assertEqual(recipes['stereo_weave']['stereo_ratio'],.67)
        self.assertLess(recipes['stereo_weave']['stereo_ratio'],1)
        self.assertAlmostEqual(recipes['stereo_weave']['stereo_ratio'],2/3,delta=.005)
        self.assertTrue(recipes['high_spark']['exciter_highpass_enabled'])
        self.assertEqual(recipes['high_spark']['feedback_highpass_hz'],300)
        self.assertFalse(any('against' in key for key in recipes))
        self.assertEqual(recipes['analog_bounce']['tone_hz'],3000)
        self.assertEqual(recipes['tape_space']['tone_hz'],4100)
        self.assertEqual(recipes['tape_space']['division'],'1/8')
        self.assertEqual(recipes['platter']['tone_hz'],3200)
        self.assertEqual(recipes['platter']['division'],'1/4')
        self.assertEqual(recipes['platter']['drive'],.428)
        self.assertFalse(recipes['platter']['modulation_enabled'])
        self.assertEqual(recipes['platter']['mod_depth_ms'],0)
        self.assertEqual(recipes['platter']['drift_amount'],0)
        for key in ('analog_bounce','tape_space'):
            self.assertEqual(recipes[key]['drive'],.428)
            self.assertEqual(recipes[key]['mod_rate_hz'],1)
            self.assertEqual(recipes[key]['mod_depth_ms'],6)
        self.assertEqual(recipes['crushed_8bit']['tone_hz'],3500)
        self.assertEqual(recipes['crushed_8bit']['lores_cutoff_hz'],2000)
        self.assertFalse(recipes['crushed_8bit']['wet_tone_enabled'])

    def test_coloured_stereo_recipes_keep_their_engines_and_independent_notes(self):
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        recipes={p['id']:{**bank['defaults'],**p['parameters']} for p in bank['presets']}
        quarter_notes={'1/4':1,'1/8':.5,'1/8D':.75,'1/4T':2/3}
        expected={
            'analog_stereo':('analog','a04','1/8D','1/4',375,500),
            'tube_stereo':('tube','b06','1/4','1/4T',500,1000/3),
            'tape_stereo':('tape','a04','1/8','1/8D',250,375),
        }
        for key,(model,voicing,left,right,left_ms,right_ms) in expected.items():
            with self.subTest(recipe=key):
                recipe=recipes[key]
                self.assertTrue(recipe['stereo_timing_enabled'])
                self.assertEqual(recipe['model'],model)
                self.assertEqual(recipe['voicing'],voicing)
                self.assertEqual((recipe['division'],recipe['right_division']),(left,right))
                self.assertAlmostEqual(60000/recipe['tempo_bpm']*quarter_notes[left],left_ms)
                self.assertAlmostEqual(60000/recipe['tempo_bpm']*quarter_notes[right],right_ms)
                self.assertAlmostEqual(recipe['stereo_ratio'],right_ms/left_ms,delta=.005)
        for key,recipe in recipes.items():
            if key not in expected:
                self.assertFalse(recipe['stereo_timing_enabled'],key)
        self.assertEqual(recipes['tape_stereo']['mod_rate_hz'],.25)
        self.assertEqual(recipes['tape_stereo']['mod_depth_ms'],.6)

    def test_character_drive_settings_leave_inactive_controls_unchanged(self):
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        recipes={p['id']:{**bank['defaults'],**p['parameters']} for p in bank['presets']}
        expected={
            'analog_bounce':.428,'tape_space':.428,'tube_slap':.482,
            'high_spark':.375,'filter_lift':.107,'filter_fall':.107,
            'platter':.428,'analog_stereo':.428,'tube_stereo':.375,'tape_stereo':.428,
        }
        for key,drive in expected.items():
            with self.subTest(recipe=key):
                self.assertEqual(recipes[key]['drive'],drive)
                self.assertEqual(generator.nt_number(drive,1000)/1000,drive)
        # The 8bit quantizer does not use Drive; a stored value is retained.
        self.assertEqual(recipes['crushed_8bit']['drive'],.4)
        for key,recipe in recipes.items():
            if key not in expected and key!='crushed_8bit':
                self.assertEqual(recipe['drive'],0,key)

    def test_swell_is_a_separate_slow_fully_wet_recipe(self):
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        recipes={p['id']:{**bank['defaults'],**p['parameters']} for p in bank['presets']}
        swell=recipes['swell'];bloom=recipes['bloom']
        self.assertEqual(swell['model'],'autovolume')
        self.assertEqual(swell['voicing'],'a05')
        self.assertEqual(swell['mix'],1)
        self.assertEqual(swell['drive'],0)
        self.assertEqual(swell['division'],'1/4')
        self.assertEqual(swell['swell_ms'],1200)
        self.assertEqual(swell['feedback'],.28)
        self.assertEqual(swell['tone_hz'],4200)
        self.assertEqual((swell['attack_ms'],swell['release_ms'],swell['duck_threshold_db']),(2,500,-40))
        self.assertTrue(swell['wet_tone_enabled'])
        self.assertFalse(swell['modulation_enabled'])
        self.assertEqual(swell['drift_amount'],0)
        self.assertEqual(bloom['division'],'1/16')
        self.assertEqual(bloom['swell_ms'],600)
        self.assertEqual(bloom['feedback'],.22)
        self.assertEqual(bank['default_preset'],'analog_bounce')

    def test_published_factory_slots_do_not_shift_when_recipes_retire(self):
        bank=json.loads((ROOT/'models/preset_bank.json').read_text(encoding='utf-8'))
        expected={'clean_quarter':1,'wide_dotted':2,'clocked_grain':3,'analog_bounce':4,
                  'tape_space':5,'tube_slap':6,'ducked_voice':7,'bloom':8,'reverse_pad':9,
                  'stereo_weave':10,'pingpong_width':11,'crushed_8bit':12,'high_spark':13,
                  'loop_bar':14,'filter_lift':18,'filter_fall':19,'platter':20,
                  'analog_stereo':21,'tube_stereo':22,'tape_stereo':23,'swell':24}
        self.assertEqual({p['id']:p['slot'] for p in bank['presets']},expected)
        self.assertEqual(bank['slot_count'],24)
        self.assertEqual(bank['retired_slots'],[15,16,17])
        header=(ROOT/'src/nt/factory_presets.h').read_text(encoding='utf-8')
        body=re.search(r'inline constexpr Preset presets\[slotCount\]=\{(.*?)\n\};',header,re.S).group(1)
        rows=[line.strip() for line in body.splitlines() if line.strip()]
        self.assertEqual(len(rows),24)
        for slot in bank['retired_slots']:self.assertEqual(rows[slot-1],'{"Custom",nullptr,0},')
        self.assertIn('"Filter Lift"',rows[17])
        self.assertIn('"Filter Fall"',rows[18])
        self.assertIn('"Platter"',rows[19])
        self.assertIn('"Analog Stereo"',rows[20])
        self.assertIn('"Tube Stereo"',rows[21])
        self.assertIn('"Tape Stereo"',rows[22])
        self.assertIn('"Swell"',rows[23])
        active=re.search(r'activeSlots\[count\]=\{([^}]+)\}',header).group(1)
        self.assertEqual([int(slot) for slot in active.split(',')],sorted(expected.values()))

    def test_stale_profiles_header_and_extra_files_are_detected_without_writing(self):
        with tempfile.TemporaryDirectory(prefix='echolab-presets-') as temporary:
            root=Path(temporary)
            (root/'models').mkdir()
            (root/'models/preset_bank.json').write_bytes((ROOT/'models/preset_bank.json').read_bytes())
            outputs=generator.bank_outputs(root)
            self.assertEqual(len(generator.stale_outputs(outputs,root)),22)
            for path,text in outputs.items():
                path.parent.mkdir(parents=True,exist_ok=True)
                path.write_text(text,encoding='utf-8')
            self.assertEqual(generator.stale_outputs(outputs,root),[])
            changed=root/'models/presets/clean_quarter.profile'
            changed.write_text('feedback=0.99\n',encoding='utf-8')
            header=root/'src/nt/factory_presets.h'
            header.write_text('// stale\n',encoding='utf-8')
            extra=root/'models/presets/unlisted.profile'
            extra.write_text('mix=0\n',encoding='utf-8')
            self.assertEqual(set(generator.stale_outputs(outputs,root)),{
                'models/presets/clean_quarter.profile','models/presets/unlisted.profile','src/nt/factory_presets.h'})
            self.assertEqual(changed.read_text(),'feedback=0.99\n')
            self.assertEqual(header.read_text(),'// stale\n')


if __name__=='__main__':
    unittest.main()
