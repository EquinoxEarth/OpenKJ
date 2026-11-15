import json
from pathlib import Path
import tempfile
import unittest

from prepare_release import digest
from publish_release import publish


VERSION = '3.0.1'
TAG = 'v' + VERSION
COMMIT = 'a' * 40
RUN_ID = '12345'


class MockReleases:
    def __init__(self):
        self.commit = COMMIT
        self.draft = None
        self.uploads = []
        self.remote = {}
        self.interrupt_after = None
        self.latest = None
        self.corrupt_upload = False

    def tag_commit(self, tag):
        return self.commit

    def find(self, tag):
        return self.draft

    def create(self, tag, commit, title, body, preview):
        self.draft = dict(id=1, draft=True, tag_name=tag, target_commitish=commit, name=title,
                          body=body, prerelease=preview)
        return self.draft

    def assets(self, release_id):
        return list(self.remote.values())

    def delete_starter(self, asset_id):
        name = next(name for name, asset in self.remote.items() if asset.get('id') == asset_id)
        assert self.remote[name]['state'] == 'starter'
        del self.remote[name]

    def upload(self, tag, path):
        if len(self.uploads) == self.interrupt_after:
            raise OSError('interrupted upload')
        assert path.name not in self.remote
        self.uploads.append(path.name)
        checksum = '0' * 64 if self.corrupt_upload else digest(path)
        self.remote[path.name] = dict(name=path.name, state='uploaded', digest='sha256:' + checksum)

    def publish(self, release_id, latest):
        self.latest = latest
        self.draft['draft'] = False
        return self.draft


class PublishReleaseTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.client = MockReleases()
        prefix = f'OpenKJ-{VERSION}-'
        packages = {}
        for suffix in ('macos-arm64.dmg', 'windows-x64.zip', 'windows-x64.msi',
                       'linux-fedora44-x86_64-portable.tar.gz', 'linux-x86_64.flatpak'):
            path = self.root / (prefix + suffix)
            path.write_text(suffix)
            packages[path.name] = digest(path)
        (self.root / (prefix + 'build-info.json')).write_text(json.dumps({
            'tag': TAG, 'version': VERSION, 'source_commit': COMMIT, 'packages': packages,
        }))
        (self.root / (prefix + 'linux-build-manifest.json')).write_text('{}')
        (self.root / (prefix + 'linux-runtime-versions.txt')).write_text('versions')
        (self.root / 'RELEASE_NOTES.md').write_text(f'# OpenKJ {VERSION}\n')
        (self.root / 'SHA256SUMS').write_text(''.join(
            f'{digest(path)}  {path.name}\n' for path in sorted(self.root.iterdir())))
        for name, checksum in packages.items():
            (self.root / (name + '.sha256')).write_text(f'{checksum}  {name}\n')
        self.manifest_sha = digest(self.root / 'SHA256SUMS')

    def promote(self, run_id=RUN_ID):
        return publish(self.client, self.root, TAG, COMMIT, run_id, self.manifest_sha)

    def interrupt(self):
        self.client.interrupt_after = 2
        with self.assertRaisesRegex(OSError, 'interrupted upload'):
            self.promote()
        self.client.interrupt_after = None

    def test_clean_publish(self):
        self.assertFalse(self.promote()['draft'])
        self.assertEqual(set(self.client.remote), {path.name for path in self.root.iterdir()})
        self.assertTrue(self.client.latest)

    def test_resumes_partial_upload_without_overwriting(self):
        self.interrupt()
        first_uploads = self.client.uploads.copy()
        self.assertTrue(self.client.draft['draft'])
        self.assertFalse(self.promote()['draft'])
        self.assertEqual(self.client.uploads[:2], first_uploads)
        self.assertEqual(len(self.client.uploads), len(set(self.client.uploads)))

    def test_resumes_empty_upload_placeholder(self):
        self.interrupt()
        self.client.remote['SHA256SUMS'] = dict(id=10, name='SHA256SUMS', state='starter', size=0)
        self.assertFalse(self.promote()['draft'])
        self.assertEqual(self.client.remote['SHA256SUMS']['state'], 'uploaded')

    def test_rejects_draft_for_different_candidate(self):
        self.interrupt()
        with self.assertRaisesRegex(ValueError, 'does not match the reviewed candidate'):
            self.promote('67890')
        self.assertEqual(len(self.client.uploads), 2)

    def test_rejects_already_published_tag(self):
        self.promote()
        with self.assertRaisesRegex(ValueError, 'already published'):
            self.promote()

    def test_rejects_changed_local_or_remote_assets(self):
        self.interrupt()
        self.client.remote[self.client.uploads[0]]['digest'] = 'sha256:' + '0' * 64
        with self.assertRaisesRegex(ValueError, 'Draft asset does not match'):
            self.promote()
        (self.root / 'RELEASE_NOTES.md').write_text('edited')
        with self.assertRaisesRegex(ValueError, 'Candidate checksum mismatch'):
            self.promote()
        self.assertEqual(len(self.client.uploads), 2)

    def test_checks_complete_remote_manifest_before_publication(self):
        self.client.corrupt_upload = True
        with self.assertRaisesRegex(ValueError, 'Draft asset does not match'):
            self.promote()
        self.assertTrue(self.client.draft['draft'])

    def test_rejects_tag_moved_to_different_commit(self):
        self.client.commit = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'does not point to the reviewed commit'):
            self.promote()
        self.assertIsNone(self.client.draft)

    def test_rejects_unreviewed_manifest(self):
        self.manifest_sha = '0' * 64
        with self.assertRaisesRegex(ValueError, 'does not match the reviewed SHA-256'):
            self.promote()
        self.assertIsNone(self.client.draft)
