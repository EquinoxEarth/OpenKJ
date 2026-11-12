"""Exercise release integrity gates without downloading or publishing assets."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('prepare_release', Path(__file__).parents[1] / 'prepare_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = self.root / 'inputs'
        self.output = self.root / 'output'
        self.commit = 'a' * 40
        self.version = release.re.search(r'OKJ_VERSION_STRING "([^"\n]+)"',
            (Path(__file__).parents[2] / 'src/okjversion.h').read_text())[1]
        self.tag = f'v{self.version}'
        for name in ('macos', 'windows-zip', 'windows-msi', 'linux', 'linux-portable'):
            (self.inputs / name).mkdir(parents=True)
        self.zip = self.inputs / 'windows-zip/OpenKJ-Windows-x64-portable.zip'
        with zipfile.ZipFile(self.zip, 'w') as archive:
            archive.writestr('OpenKJ-Windows-x64-portable/manifest.json', json.dumps({'commit': self.commit}))
        self.package = self.inputs / f'windows-msi/OpenKJ-{self.version}-windows-x64.msi'
        self.portable = self.inputs / 'linux-portable/OpenKJ-Fedora44-x86_64-portable.tar.gz'
        self.write_portable(self.commit)
        files = [self.zip, self.package, self.portable,
                 self.inputs / f'macos/OpenKJ-{self.version}-macos-arm64.dmg',
                 self.inputs / 'linux/OpenKJ-Linux-x86_64.flatpak']
        for path in files:
            if not path.exists():
                path.write_bytes(b'package fixture')
            Path(str(path) + '.sha256').write_text(f'{release.digest(path)}  {path.name}\n')
        (self.inputs / 'linux/build-manifest.json').write_text(json.dumps({'openkj-source-commit': self.commit}))
        self.branch = self.inputs / 'linux/branch.txt'
        self.branch.write_text('stable\n')
        (self.inputs / 'linux/runtime-versions.txt').write_text('org.kde.Platform 6.11\n')
        (self.inputs / 'macos/RELEASE_NOTES.md').write_text('https://github.com/EquinoxEarth/OpenKJ/commit/' + self.commit)

    def write_portable(self, commit):
        source_commit = self.root / 'source-commit.txt'
        source_commit.write_text(commit + '\n')
        with tarfile.open(self.portable, 'w:gz') as archive:
            archive.add(source_commit, arcname='./source-commit.txt')
        Path(str(self.portable) + '.sha256').write_text(
            f'{release.digest(self.portable)}  {self.portable.name}\n')

    def stage(self):
        release.prepare(self.inputs, self.output, self.tag, self.commit)

    def test_complete_release_checksums_and_versioned_names(self):
        self.stage()
        self.assertTrue((self.output / f'OpenKJ-{self.version}-windows-x64.zip').exists())
        self.assertTrue((self.output / f'OpenKJ-{self.version}-linux-x86_64.flatpak').exists())
        info = json.loads((self.output / f'OpenKJ-{self.version}-build-info.json').read_text())
        self.assertEqual(info['tag'], self.tag)
        self.assertEqual(len(info['packages']), 5)
        self.assertIn(f'OpenKJ-{self.version}-linux-fedora44-x86_64-portable.tar.gz', info['packages'])
        for line in (self.output / 'SHA256SUMS').read_text().splitlines():
            expected, name = line.split()
            self.assertEqual(expected, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_corrupted_package_never_staged(self):
        self.package.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'Checksum mismatch'):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_legacy_preview_tags_remain_supported(self):
        self.branch.write_text('preview\n')
        for channel in ('prerelease', 'preview', 'beta'):
            with self.subTest(channel=channel):
                self.tag = f'v{self.version}-{channel}.1'
                self.output = self.root / channel
                self.stage()
                self.assertTrue((self.output / 'SHA256SUMS').exists())

    def test_invalid_beta_tags_never_staged(self):
        for suffix in ('beta', 'beta.x', 'stable.1'):
            with self.subTest(suffix=suffix):
                self.tag = f'v{self.version}-{suffix}'
                with self.assertRaisesRegex(ValueError, 'Expected a stable or prerelease tag'):
                    self.stage()
                self.assertFalse(self.output.exists())

    def test_mixed_source_commits_never_staged(self):
        (self.inputs / 'linux/build-manifest.json').write_text(json.dumps({'openkj-source-commit': 'b' * 40}))
        with self.assertRaisesRegex(ValueError, 'source commit'):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_mixed_portable_commit_never_staged(self):
        self.write_portable('b' * 40)
        with self.assertRaisesRegex(ValueError, 'Linux portable source commit'):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_wrong_flatpak_branch_never_staged(self):
        self.branch.write_text('preview\n')
        with self.assertRaisesRegex(ValueError, 'Flatpak branch'):
            self.stage()
        self.assertFalse(self.output.exists())

    def test_wrong_tag_rejected(self):
        self.tag = 'v0.0.1-prerelease.1'
        with self.assertRaisesRegex(ValueError, 'source version'):
            self.stage()

    def test_existing_output_never_overwritten(self):
        self.stage()
        with self.assertRaises(FileExistsError):
            self.stage()


if __name__ == '__main__':
    unittest.main()
