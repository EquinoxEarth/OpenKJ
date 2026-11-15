#!/usr/bin/env python3
"""Publish the exact assets from a reviewed OpenKJ release candidate."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

from prepare_release import digest


def candidate_assets(directory, tag, commit, manifest_sha):
    manifest = directory / 'SHA256SUMS'
    if digest(manifest) != manifest_sha:
        raise ValueError('Candidate manifest does not match the reviewed SHA-256')

    version = tag[1:].split('-')[0]
    prefix = f'OpenKJ-{version}-'
    packages = {prefix + suffix for suffix in (
        'macos-arm64.dmg', 'windows-x64.zip', 'windows-x64.msi',
        'linux-fedora44-x86_64-portable.tar.gz', 'linux-x86_64.flatpak',
    )}
    info_name = prefix + 'build-info.json'
    expected = packages | {info_name, 'RELEASE_NOTES.md',
                           prefix + 'linux-build-manifest.json', prefix + 'linux-runtime-versions.txt'}
    checksums = {}
    for line in manifest.read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._-]+)', line)
        if not match or match[2] not in expected or match[2] in checksums:
            raise ValueError(f'Invalid candidate manifest entry: {line!r}')
        checksums[match[2]] = match[1]
    if checksums.keys() != expected:
        raise ValueError('Candidate manifest is missing release files')

    for name in expected:
        if digest(directory / name) != checksums[name]:
            raise ValueError(f'Candidate checksum mismatch: {name}')

    info = json.loads((directory / info_name).read_text())
    if (info.get('version'), info.get('tag'), info.get('source_commit'), info.get('packages')) != (
            version, tag, commit, {name: checksums[name] for name in packages}):
        raise ValueError('Candidate build information does not match the release tag, commit and packages')

    for name in packages:
        sidecar = directory / (name + '.sha256')
        if sidecar.read_bytes() != f'{checksums[name]}  {name}\n'.encode('ascii'):
            raise ValueError(f'Invalid checksum file: {sidecar.name}')
        checksums[sidecar.name] = digest(sidecar)

    checksums[manifest.name] = manifest_sha
    if {path.name for path in directory.iterdir()} != checksums.keys():
        raise ValueError('Candidate contains unexpected files')
    return checksums


def gh(*args, input_data=None):
    return subprocess.run(['gh', *args], input=input_data, text=True, check=True,
                          stdout=subprocess.PIPE).stdout


class GitHubReleases:
    def __init__(self, repo):
        self.repo = repo
        self.base = f'repos/{repo}'

    def api(self, path, *flags, payload=None):
        args = ['api', f'{self.base}/{path}', *flags]
        if payload is not None:
            args += ['--input', '-']
        return json.loads(gh(*args, input_data=json.dumps(payload) if payload is not None else None))

    def paginated(self, path):
        return [item for page in self.api(path + '?per_page=100', '--paginate', '--slurp') for item in page]

    def tag_commit(self, tag):
        return self.api(f'commits/{quote("refs/tags/" + tag, safe="")}')['sha']

    def find(self, tag):
        matches = [release for release in self.paginated('releases') if release['tag_name'] == tag]
        if len(matches) > 1:
            raise ValueError(f'Multiple releases found for {tag}')
        return next(iter(matches), None)

    def create(self, tag, commit, title, body, preview):
        return self.api('releases', '--method', 'POST', payload={
            'tag_name': tag, 'target_commitish': commit, 'name': title, 'body': body,
            'draft': True, 'prerelease': preview, 'make_latest': 'false',
        })

    def assets(self, release_id):
        return self.paginated(f'releases/{release_id}/assets')

    def delete_starter(self, asset_id):
        gh('api', f'{self.base}/releases/assets/{asset_id}', '--method', 'DELETE')

    def upload(self, tag, path):
        gh('release', 'upload', tag, str(path.resolve()), '--repo', self.repo)

    def publish(self, release_id, latest):
        return self.api(f'releases/{release_id}', '--method', 'PATCH', payload={
            'draft': False, 'make_latest': 'true' if latest else 'false',
        })


def verified_assets(remote, expected):
    names = set()
    for asset in remote:
        name = asset['name']
        if (name in names or name not in expected or asset['state'] != 'uploaded'
                or asset.get('digest') != 'sha256:' + expected[name]):
            raise ValueError(f'Draft asset does not match the candidate: {name}')
        names.add(name)
    return names


def publish(client, directory, tag, commit, run_id, manifest_sha):
    match = re.fullmatch(r'v(\d+\.\d+\.\d+)(?:-(beta|preview|prerelease)\.\d+)?', tag)
    if (not match or not re.fullmatch(r'[0-9a-f]{40}', commit) or
            not re.fullmatch(r'[1-9][0-9]*', run_id) or not re.fullmatch(r'[0-9a-f]{64}', manifest_sha)):
        raise ValueError('Expected a release tag, full source commit, candidate run ID and manifest SHA-256')
    expected = candidate_assets(directory, tag, commit, manifest_sha)
    if client.tag_commit(tag) != commit:
        raise ValueError('The release tag does not point to the reviewed commit')

    version, channel = match.groups()
    preview = channel in {'preview', 'prerelease'}
    title = f'{version} Beta' if channel == 'beta' else f'OpenKJ {version}'
    if preview:
        title += ' Preview'
    marker = f'<!-- openkj-release candidate={run_id} commit={commit} manifest={manifest_sha} -->'
    body = (directory / 'RELEASE_NOTES.md').read_text().rstrip() + f'\n\n{marker}\n'

    def verify_draft(draft):
        if not draft['draft']:
            raise ValueError(f'{tag} is already published')
        if (draft['tag_name'] != tag or draft['name'] != title or draft['prerelease'] != preview or
                (draft['body'] or '').strip() != body.strip()):
            raise ValueError(f'Existing draft for {tag} does not match the reviewed candidate')

    draft = client.find(tag)
    if draft is None:
        draft = client.create(tag, commit, title, body, preview)
    verify_draft(draft)
    release_id = draft['id']
    remote = client.assets(release_id)
    starters = [asset for asset in remote if asset['state'] == 'starter'
                and asset['size'] == 0 and not asset.get('digest')]
    uploaded = verified_assets([asset for asset in remote if asset not in starters], expected)
    names = [asset['name'] for asset in starters]
    if len(set(names)) != len(names) or set(names) - expected.keys() or set(names) & uploaded:
        raise ValueError('Draft contains unexpected incomplete assets')
    for asset in starters:
        client.delete_starter(asset['id'])
    for name in sorted(expected.keys() - uploaded):
        client.upload(tag, directory / name)

    final_draft = client.find(tag)
    if not final_draft or final_draft['id'] != release_id:
        raise ValueError('Release draft changed during upload')
    verify_draft(final_draft)
    if client.tag_commit(tag) != commit:
        raise ValueError('The release tag moved during upload')
    if verified_assets(client.assets(release_id), expected) != expected.keys():
        raise ValueError('Draft does not contain every candidate asset')
    return client.publish(release_id, latest=not preview)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--manifest-sha256', required=True)
    args = parser.parse_args()
    try:
        release = publish(GitHubReleases(args.repo), args.directory, args.tag, args.commit,
                          args.run_id, args.manifest_sha256)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
    print(release['html_url'])
