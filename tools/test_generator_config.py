"""Synthetic section layouts; no borrowed game memory maps."""
import importlib
import unittest
from tools.xboxrecomp.recomp import config


class GeneratorConfigTests(unittest.TestCase):
    def tearDown(self):
        importlib.reload(config)

    def test_starts_without_an_embedded_game_layout(self):
        importlib.reload(config)
        self.assertEqual(config.SECTIONS, [])
        self.assertIsNone(config.configured_from())

    def test_section_and_bss_mapping(self):
        sections = [config.Section('.text', 0x10000, 32, 64, 16, True),
                    config.Section('.data', 0x20000, 16, 80, 8, False)]
        config._install(sections, 0x10000, 0x20000, 'synthetic')
        self.assertEqual(config.va_to_file_offset(0x10004), 68)
        self.assertIsNone(config.va_to_file_offset(0x10014))
        self.assertTrue(config.is_code_address(0x10004))
        self.assertFalse(config.is_code_address(0x20004))

    def test_second_image_resets_absent_data_and_thunks(self):
        config._install([config.Section('.data', 0x20000, 16, 80, 8, False)], 7, 9, 'first')
        config._install([config.Section('.text', 0x10000, 32, 64, 16, True)], 0, 0, 'second')
        self.assertEqual(config.RDATA_VA_START, 0)
        self.assertEqual(config.DATA_VA_START, 0)
        self.assertEqual(config.KERNEL_THUNK_ADDR, 0)
        self.assertEqual(config.ENTRY_POINT, 0)


    def test_xadd_declares_snapshots_without_a_branch_or_compare(self):
        from tools.xboxrecomp.recomp.translator import FunctionTranslator
        # Synthetic x86: xadd [ecx], eax; ret (also its LOCK form).
        for code in (bytes.fromhex('0fc101c3'), bytes.fromhex('f00fc101c3')):
            config._install([config.Section('.text', 0x10000, len(code), 0, len(code), True)],
                            0x10000, 0, 'synthetic')
            info = dict(_addr=0x10000, start=0x10000, end=0x10000+len(code), name='synthetic_xadd')
            translator = FunctionTranslator(code, {0x10000: info}, seh_prolog=set(), seh_epilog=set())
            emitted = translator.translate_function(0x10000, info)
            self.assertIn('uint32_t _fa = 0, _fb = 0;', emitted)
            self.assertIn('int32_t _fas = 0, _fbs = 0;', emitted)
            self.assertIn('_fa = _xa_d; _fb = _xa_s;', emitted)

    def test_configured_entry_realigns_an_overlapping_sweep(self):
        from tools.xboxrecomp.disasm.loader import BinaryImage, SectionInfo
        from tools.xboxrecomp.disasm.engine import DisasmEngine
        from tools.xboxrecomp.disasm.functions import FunctionDetector
        from tools.xboxrecomp.disasm.labels import LabelManager
        from tools.xboxrecomp.disasm.xrefs import XRefTracker
        # An artificial data byte before push ebp; mov ebp,esp; ret makes
        # the sweep decode one mov-immediate across the whole function.
        code = bytes.fromhex('b8558becc3')
        section = SectionInfo('.text', 0x20000, 5, 0, 5, False, True, '')
        image = BinaryImage('synthetic', code, 0, 5, 0, 0, [section])
        engine = DisasmEngine(image)
        engine.linear_sweep(section)
        self.assertIsNone(engine.get_instruction(0x20001))
        detector = FunctionDetector(engine, image, XRefTracker(), LabelManager())
        detector.add_configured_functions([0x20001])
        detector._build_functions([section])
        self.assertEqual(detector.functions[0x20001].end, 0x20005)
        with self.assertRaisesRegex(ValueError, 'Cannot decode'):
            detector.add_configured_functions([0x30000])


if __name__ == '__main__':
    unittest.main()
