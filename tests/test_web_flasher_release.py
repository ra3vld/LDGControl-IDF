"""Verify that a site-only rebuild preserves the complete release payload."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import web_flasher_release as release


class WebsiteRelease(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.site = self.root / 'site'
        self.builds = self.root / 'builds'
        self.assets = self.root / 'release'
        self.commit = subprocess.check_output(
            ['git', 'rev-parse', 'HEAD'], cwd=release.ROOT, text=True,
        ).strip()
        profiles = []
        for board in release.BOARDS:
            elf = self.builds / f'build-{board}' / 'ldgcontrol.elf'
            elf.parent.mkdir(parents=True)
            elf.write_bytes(f'ELF for {board}'.encode())
            profiles.append({'id': board, 'build': {
                'commit': self.commit, 'version': '1.2.3', 'source_digest': 'a' * 64,
                'elf_sha256': release.sha(elf.read_bytes()),
            }})
            firmware = self.site / 'firmware' / board
            firmware.mkdir(parents=True)
            with ZipFile(firmware / 'usb-firmware.zip', 'w') as archive:
                for name in sorted(release.USB_FILES):
                    data = f'{board}/{name}'.encode()
                    (firmware / name).write_bytes(data)
                    archive.writestr(name, data)
        (self.site / 'catalog.json').write_text(json.dumps({
            'schema': 1, 'commit': self.commit, 'profiles': profiles,
        }))
        (self.site / 'index.html').write_text('Tagged installer')
        (self.site / 'sources.zip').write_bytes(b'tagged sources')
        with patch.object(release, 'check_tag', return_value='1.2.3'):
            release.package('v1.2.3', self.site, self.builds, self.assets, signing_key='')

    def test_site_update_preserves_firmware_and_sources(self):
        output = self.root / 'updated'
        release.restore('v1.2.3', self.assets, output, allow_dirty=True)
        for path in self.site.rglob('*'):
            if path.is_file() and path.name != 'index.html':
                self.assertEqual(path.read_bytes(), (output / path.relative_to(self.site)).read_bytes())
        self.assertEqual((output / 'index.html').read_bytes(),
                         (release.ROOT / 'web-flasher/index.html').read_bytes())
        identity = json.loads((output / 'site-identity.json').read_text())
        self.assertEqual(identity['firmware_tag'], 'v1.2.3')
        self.assertEqual(identity['firmware_commit'], self.commit)
        with ZipFile(self.assets / release.SITE_ARCHIVE) as archive:
            self.assertEqual(sorted(archive.namelist()), ['catalog.json', 'index.html'])

    def test_rejects_wrong_tag_and_modified_archive(self):
        output = self.root / 'rejected'
        with self.assertRaisesRegex(ValueError, 'version'):
            release.restore('v1.2.4', self.assets, output, allow_dirty=True)
        (self.assets / f'firmware-{release.BOARDS[0]}.zip').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            release.restore('v1.2.3', self.assets, output, allow_dirty=True)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
