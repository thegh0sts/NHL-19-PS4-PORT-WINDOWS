from paths import ROOT
import json
import struct
import tempfile
import unittest
from pathlib import Path

import patches
from patches import (EBOOT_BASE, GAME_PATCH_KEYS, compile_external, eboot_segments, encode,
                     external_patches, external_selection, read_settings, write_blob)

SEGMENTS = [(0, 0x6000000)]

EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA13795</ID></TitleID>
  <Metadata Title="CTR" Name="On" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Off" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00500010" Value="0x11223344"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Address="" Value="90 90"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500020" Value="90"/></PatchList>
  </Metadata>
</Patch>
"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_filters_version_title_and_mask_patches(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA13795', 'CUSA99999'))
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found], ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # isEnabled selects On and Mask; Mask is skipped whole for its unsupported line.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x100000, b'\x90\x90')])
            config = folder / 'patches.json'
            config.write_text(json.dumps({'enabled': ['extra.xml/Off'], 'disabled': ['extra.xml/On']}))
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x100010, b'\x44\x33\x22\x11')])

    def test_out_of_segment_patch_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'extra.xml').write_text(EXTERNAL)
            found = external_patches(folder)
            self.assertEqual(compile_external(external_selection(found, None), [(0, 0x1000)]), [])

    def test_encode_types(self):
        import xml.etree.ElementTree as ET
        line = ET.fromstring('<Line Type="float32" Address="0x400000" Value="1.0"/>')
        self.assertEqual(encode(line), struct.pack('<f', 1.0))
        line = ET.fromstring('<Line Type="utf8" Address="0x400000" Value="ab"/>')
        self.assertEqual(encode(line), b'ab\0')
        with self.assertRaises(ValueError):
            encode(ET.fromstring('<Line Type="mask" Address="" Value="90"/>'))

    def test_eboot_segments_and_blob(self):
        elf = bytearray(120)
        struct.pack_into('<Q', elf, 0x20, 64)
        struct.pack_into('<HH', elf, 0x36, 56, 1)
        struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0x1000, 0, 0, 0x2000, 0)
        self.assertEqual(eboot_segments(bytes(elf)), [(0x1000, 0x3000)])
        with tempfile.TemporaryDirectory() as tmp:
            write_blob(Path(tmp), [(0x10, b'\x90')])
            blob = (Path(tmp) / 'patches.bin').read_bytes()
            self.assertEqual(blob[:24], struct.pack('<8sQQ', b'BBPATCH2', EBOOT_BASE, 1))
            self.assertEqual(blob[24:], struct.pack('<QQ', 0x10, 1) + b'\x90')


class SegmentTests(unittest.TestCase):
    def test_executable_segment_runs_to_its_page_end(self):
        # NHL 19 1.60's layout: text ends at 0x56ea4b0, the next segment starts at 0x56ec000; patch code caves sit
        # in between (the loader maps that padding, prepare.py mapped_size).
        heads = [(1, 5, 0x56ea4b0 - 0x0, 0), (0x61000010, 4, 0x278000, 0x56ec000), (1, 6, 0x9bb060, 0x5964000),
                 (1, 6, 0x100, 0x7000000 - 0x10)]
        elf = bytearray(64 + 56 * len(heads))
        struct.pack_into('<Q', elf, 0x20, 64)
        struct.pack_into('<HH', elf, 0x36, 56, len(heads))
        for i, (kind, flags, memsz, vaddr) in enumerate(heads):
            struct.pack_into('<IIQQQQQQ', elf, 64 + 56 * i, kind, flags, 0, vaddr, vaddr, 0, memsz, 0x4000)
        self.assertEqual(eboot_segments(bytes(elf)), [(0, 0x56ec000), (0x5964000, 0x5964000 + 0x9bb060),
                                                      (0x7000000 - 0x10, 0x7000000 - 0x10 + 0x100)])
        # Short of the next segment, and of the image end.
        struct.pack_into('<IIQQQQQQ', elf, 64 + 56, 0x61000010, 4, 0, 0x56eb000, 0x56eb000, 0, 0x100, 0x4000)
        self.assertEqual(eboot_segments(bytes(elf))[0], (0, 0x56eb000))
        struct.pack_into('<IIQQQQQQ', elf, 64 + 56 * 3, 1, 5, 0, 0x7000000, 0x7000000, 0, 0x10, 0x4000)
        self.assertEqual(eboot_segments(bytes(elf))[-1], (0x7000000, 0x7000010))


def tiny_elf(code, vaddr=0x1000, offset=0x100):
    """An ELF64 header with one load segment holding code at vaddr (file offset offset)."""
    elf = bytearray(offset) + code
    struct.pack_into('<Q', elf, 0x20, 64)
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, 64, 1, 5, offset, vaddr, vaddr, len(code), len(code), 0x10)
    return bytes(elf)


class SignatureTests(unittest.TestCase):
    """Lines with a Signature: written at Address when the code is there, else where it is found."""
    SIGNED = """<?xml version="1.0"?><Patch><TitleID><ID>CUSA13795</ID></TitleID>
      <Metadata Title="CTR" Name="S" Author="x" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes" Address="{address}" Value="9090" Signature="aa bb ?? dd" SignatureOffset="0x2"/></PatchList>
      </Metadata></Patch>"""

    def writes(self, elf, address):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'p.xml').write_text(self.SIGNED.format(address=hex(address)))
            found = external_patches(Path(tmp))
            return compile_external(external_selection(found, None), eboot_segments(elf), elf)

    def test_signature_at_the_address(self):
        elf = tiny_elf(b'\0' * 16 + b'\xaa\xbb\x01\xdd' + b'\0' * 12)
        self.assertEqual(self.writes(elf, EBOOT_BASE + 0x1012), [(0x1012, b'\x90\x90')])

    def test_signature_moved(self):
        # The 6.72+ build: the same code at another address.
        elf = tiny_elf(b'\0' * 4 + b'\xaa\xbb\x07\xdd' + b'\0' * 24)
        self.assertEqual(self.writes(elf, EBOOT_BASE + 0x1012), [(0x1006, b'\x90\x90')])

    def test_signature_missing_or_ambiguous_skips_the_patch(self):
        self.assertEqual(self.writes(tiny_elf(b'\0' * 32), EBOOT_BASE + 0x1012), [])
        twice = tiny_elf(b'\xaa\xbb\x00\xdd' * 2 + b'\0' * 24)
        self.assertEqual(self.writes(twice, EBOOT_BASE + 0x1012), [])


class GamePatchTests(unittest.TestCase):
    """The bundled CTR patches, chosen through bbport.ini patch_* keys (the in-game menu). The NHL 19
    port bundles no game patch file (patches/.CrashTeamRacing.xml.disabled), so these skip."""

    def setUp(self):
        if not (ROOT / 'patches' / 'CrashTeamRacing.xml').is_file():
            self.skipTest('no bundled game patch file')

    def test_bundled_file_lists_every_menu_patch(self):
        found = external_patches(ROOT / 'patches')
        keys = {key for key, _, _ in found}
        for patch in GAME_PATCH_KEYS.values():
            self.assertIn(patch, keys)
        self.assertEqual(external_patches(ROOT / 'patches', title_id='CUSA99999'), [])
        self.assertEqual(external_patches(ROOT / 'patches', app_version='01.00'), [])

    def test_ini_keys_select_patches(self):
        found = external_patches(ROOT / 'patches')
        self.assertEqual(external_selection(found, None, {}), [])
        selected = external_selection(found, None, {'patch_60fps': '1', 'patch_no_motion_blur': '0'})
        self.assertEqual([key for key, _, _ in selected], ['CrashTeamRacing.xml/60 FPS Unlock'])
        signature = selected[0][2].find('.//Line').get('Signature')
        code = bytes(0 if b == '??' else int(b, 16) for b in signature.split())
        start = 0x017a9d8b - EBOOT_BASE  # the 5.05 build's address for the signature
        for at in (start, start + 0x40):  # 5.05 build; code moved (6.72+ build)
            elf = tiny_elf(b'\0' * (at - 0x13a9d00) + code + b'\0' * 32, vaddr=0x13a9d00)
            writes = compile_external(selected, eboot_segments(elf), elf)
            self.assertEqual(writes, [(at + 5, bytes.fromhex('41899688040000'))])

    def test_every_bundled_patch_is_signed_and_found_at_its_address(self):
        # The signatures must match at their published addresses in the firmware 5.05 build
        # (out/eboot.elf when a prepared game of that build is present; skipped otherwise).
        elf_path = ROOT / 'out' / 'eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared game in out/')
        elf = elf_path.read_bytes()
        found = external_patches(ROOT / 'patches')
        for key, _, meta in found:
            for line in meta.iter('Line'):
                self.assertTrue(line.get('Signature'), key)
                offset = int(line.get('Address'), 0) - EBOOT_BASE
                self.assertEqual(patches.locate(line, offset, elf), offset, key)

    def test_read_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / 'bbport.ini'
            ini.write_text('# comment\npatch_60fps=1\nfps_counter = 0\n')
            self.assertEqual(read_settings(ini), {'patch_60fps': '1', 'fps_counter': '0'})
            self.assertEqual(read_settings(Path(tmp) / 'missing.ini'), {})


LOCAL_RESOLUTION = """<?xml version="1.0"?>
<Patch>
  <Metadata Name="Resolution 2560x1440 (Kravickas)" AppVer="01.60" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500000" Value="0A0B"/></PatchList>
  </Metadata>
  <Metadata Name="Something else" AppVer="01.60" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500010" Value="90"/></PatchList>
  </Metadata>
</Patch>
"""


class OvertimeOnlineTests(unittest.TestCase):
    """The Overtime online set (NHL19-Overtime.xml + NHL19-Overtime-Online.xml) must make Overtime 1.00.0's
    netplay fingerprint, whatever server the host slots name."""

    def found(self, extra=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        folder = Path(tmp.name)
        for name in ('NHL19-Overtime.xml', 'NHL19-Overtime-Online.xml', 'NHL19-Online.xml'):
            (folder / name).write_bytes((ROOT / 'patches' / name).read_bytes())
        if extra:
            (folder / 'extra.xml').write_text(extra)
        return external_patches(folder, '01.60', 'CUSA11126'), folder

    def test_set_makes_overtime_fingerprint(self):
        found, _ = self.found()
        selected = patches.overtime_selection(found, None)
        self.assertEqual([key for key, _, _ in selected], [k for k, _, _ in found if k in patches.OVERTIME_ONLINE_PATCHES])
        self.assertEqual(len(selected), len(patches.OVERTIME_ONLINE_PATCHES))
        self.assertEqual(patches.netplay_fingerprint(selected), patches.OVERTIME_ONLINE_FINGERPRINT)
        # Every file of the set is off by default: the default profile applies none of the online blocks.
        default = {key for key, _, _ in external_selection(found, None)}
        self.assertFalse({k for k in patches.OVERTIME_ONLINE_PATCHES if k.startswith('NHL19-Overtime-Online.xml/')} & default)
        writes = compile_external(selected, [(0, 0x7000000)], strict=True)
        self.assertTrue(writes)

    def test_host_slots_do_not_change_the_fingerprint(self):
        found, _ = self.found()
        selected = patches.overtime_selection(found, None)
        patches.set_overtime_host(selected, '192.168.1.15')
        self.assertEqual(patches.netplay_fingerprint(selected), patches.OVERTIME_ONLINE_FINGERPRINT)
        values = {line.get('Address'): bytes.fromhex(line.get('Value'))
                  for _, _, meta in selected for line in meta.iter('Line')}
        self.assertEqual(values['0x51DCCFF'], b'192.168.1.15\0')
        for addr, _ in patches.OVERTIME_NUCLEUS_SLOTS:
            self.assertEqual(values[addr], b'https://192.168.1.15/\0')

    def test_long_host_needs_a_short_base_url(self):
        found, _ = self.found()
        selected = patches.overtime_selection(found, None)
        with self.assertRaises(ValueError):
            patches.set_overtime_host(selected, 'overtime-dev.example.org')  # https://.../ is 29 characters
        patches.set_overtime_host(selected, 'overtime-dev.example.org', 'https://10.0.0.2/')
        values = {line.get('Address'): bytes.fromhex(line.get('Value'))
                  for _, _, meta in selected for line in meta.iter('Line')}
        self.assertEqual(values['0x51DCCFF'], b'overtime-dev.example.org\0')
        self.assertEqual(values['0x526E089'], b'https://10.0.0.2/\0')
        for bad in ('', 'a b', 'x--y', '-host', 'host/'):
            with self.assertRaises(ValueError):
                patches.set_overtime_host(selected, bad)
        with self.assertRaises(ValueError):
            patches.set_overtime_host(selected, 'host', 'ftp://x/')

    def test_launcher_choices_keep_only_local_patches(self):
        found, folder = self.found(LOCAL_RESOLUTION)
        config = folder / 'patches.json'
        config.write_text(json.dumps({'enabled': ['NHL19-Online.xml/Blaze console login (login_with_nucleussdk off)'],
                                      'disabled': list(patches.OVERTIME_ONLINE_PATCHES)}))
        selected = patches.overtime_selection(found, config)
        keys = [key for key, _, _ in selected]
        self.assertIn('extra.xml/Resolution 2560x1440 (Kravickas)', keys)
        self.assertNotIn('extra.xml/Something else', keys)
        self.assertNotIn('NHL19-Online.xml/Blaze console login (login_with_nucleussdk off)', keys)
        for key in patches.OVERTIME_ONLINE_PATCHES:
            self.assertIn(key, keys)  # patches.json cannot turn the set off
        self.assertEqual(patches.netplay_fingerprint(selected), patches.OVERTIME_ONLINE_FINGERPRINT)

    def test_missing_or_failing_patch_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'NHL19-Overtime.xml').write_bytes((ROOT / 'patches' / 'NHL19-Overtime.xml').read_bytes())
            with self.assertRaises(ValueError):
                patches.overtime_selection(external_patches(Path(tmp), '01.60', 'CUSA11126'), None)
        found, _ = self.found()
        selected = patches.overtime_selection(found, None)
        with self.assertRaises(ValueError):
            compile_external(selected, [(0, 0x1000)], strict=True)
        self.assertEqual(compile_external(selected, [(0, 0x1000)]), [])

    def test_fingerprint_rule(self):
        # Line order, comments and case do not count; local-only patches and host slots do not count.
        import xml.etree.ElementTree as ET
        def meta(name, *lines):
            m = ET.fromstring(f'<Metadata Name="{name}"><PatchList>' + ''.join(
                f'<Line Type="bytes" Address="{a}" Value="{v}"/>' for a, v in lines) + '</PatchList></Metadata>')
            return ('f.xml/' + name, None, m)
        a = [meta('A', ('0x500000', '90'), ('0x500010', 'ab'))]
        b = [meta('A', ('0x500010', 'AB'), ('0x500000', '90')), meta('60 FPS replays (Kravickas)', ('0x600000', '01')),
             meta('Host', ('0x51dccff', '6100'))]
        self.assertEqual(patches.netplay_fingerprint(a), patches.netplay_fingerprint(b))
        import hashlib
        self.assertEqual(patches.netplay_fingerprint(a), hashlib.sha256(b'0X500000=90\n0X500010=AB').hexdigest()[:20])

    def test_main_writes_fingerprint_and_fails_whole(self):
        import subprocess
        import sys
        elf = tiny_elf(b'\0' * 0x100, vaddr=0)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'eboot.elf').write_bytes(elf)
            env = dict(__import__('os').environ, BB_ONLINE_PROFILE='overtime')
            run = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'patches.py'), '--patches-dir', str(ROOT / 'patches'),
                                  '--app-version', '01.60', '--title-id', 'CUSA11126', '--out', str(out),
                                  '--settings', str(out / 'none.ini')], env=env, capture_output=True, text=True)
            # The set's addresses lie outside this tiny eboot: an error, and no blob or fingerprint left behind.
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('Overtime online', run.stderr)
            self.assertFalse((out / 'patches.bin').exists())
            self.assertFalse((out / 'netplay_fingerprint.txt').exists())
            env['BB_ONLINE_PROFILE'] = ''
            run = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'patches.py'), '--patches-dir', str(ROOT / 'patches'),
                                  '--app-version', '01.60', '--title-id', 'CUSA11126', '--out', str(out),
                                  '--settings', str(out / 'none.ini')], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertTrue((out / 'patches.bin').exists())
            self.assertRegex((out / 'netplay_fingerprint.txt').read_text(), r'^[0-9a-f]{20}\n$')

    def test_prepared_game_takes_the_whole_set(self):
        # With a prepared NHL 19 1.60 in out/ (skipped otherwise): every write of the set lands in the eboot.
        elf_path = ROOT / 'out' / 'eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared game in out/')
        elf = elf_path.read_bytes()
        found, _ = self.found()
        selected = patches.overtime_selection(found, None)
        try:
            writes = compile_external(selected, eboot_segments(elf), elf, strict=True)
        except ValueError as error:
            self.skipTest(f'out/ holds another game ({error})')
        self.assertEqual(len(writes), sum(len(list(m.iter('Line'))) for _, _, m in selected))


if __name__ == '__main__':
    unittest.main()
