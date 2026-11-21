#!/usr/bin/env python3
"""Package a tagged USB release and restore its firmware for a Pages update."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from web_flasher import PROFILES, ROOT, prepare_site, sha

BOARDS = tuple(profile['id'] for profile in PROFILES)
USB_FILES = {
    'bootloader.bin', 'partition-table.bin', 'ota_data_initial.bin', 'ldgcontrol.bin',
    'manifest.json', 'checksums.json',
}
SITE_ARCHIVE = 'web-flasher-site.zip'
SOURCE_ARCHIVE = 'sources.zip'


def tag_version(tag):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', tag) or len(tag) > 32:
        raise ValueError('Release tag must be vMAJOR.MINOR.PATCH and fit the ESP-IDF version field')
    return tag[1:]


def check_tag(tag):
    version = tag_version(tag)

    def git(*args):
        return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()

    commit = git('rev-parse', 'HEAD')
    if git('rev-parse', 'refs/tags/' + tag + '^{commit}') != commit:
        raise ValueError('Release tag must point to the checked-out commit')
    if git('status', '--porcelain'):
        raise ValueError('Release requires a clean checkout')
    subprocess.run(['git', 'merge-base', '--is-ancestor', commit, 'refs/remotes/origin/main'],
                   cwd=ROOT, check=True)
    return version


def read_catalog(data, tag):
    version = tag_version(tag)
    catalog = json.loads(data)
    profiles = catalog['profiles']
    if catalog['schema'] != 1 or sorted(p['id'] for p in profiles) != sorted(BOARDS):
        raise ValueError('Release must contain the complete board catalog')
    commit = catalog['commit']
    builds = [profile['build'] for profile in profiles]
    if (not re.fullmatch(r'[0-9a-f]{40}', commit)
            or any(build['commit'] != commit or build['version'] != version for build in builds)
            or len({build['source_digest'] for build in builds}) != 1
            or not re.fullmatch(r'[0-9a-f]{64}', builds[0]['source_digest'])):
        raise ValueError('Catalog commit, version or firmware source identity mismatch')
    return catalog


def write_zip(target, root, paths):
    with ZipFile(target, 'x', ZIP_DEFLATED) as archive:
        for path in sorted(paths):
            archive.write(path, path.relative_to(root).as_posix())


def release_notes(tag, site, output, signing_key):
    notes = (f'LDGControl-IDF {tag}\n\n'
             '[Open the USB installer](https://ra3vld.github.io/LDGControl-IDF/).\n\n'
             '- `firmware-<board>.zip` contains the USB images and flashing metadata.\n'
             '- `diagnostic-elf.zip` contains the matching ELF files.\n'
             '- `web-flasher-site.zip` and `sources.zip` contain the installer and sources.\n'
             '- `SHA256SUMS` verifies the release files.\n')
    if not signing_key:
        return notes

    with tempfile.TemporaryDirectory() as temp:
        key = Path(temp) / 'key.pem'
        key.write_text(signing_key)
        for board in BOARDS:
            subprocess.run([
                sys.executable, ROOT / 'tools/package_release.py', '--board', board,
                '--version', tag_version(tag), '--image', site / 'firmware' / board / 'ldgcontrol.bin',
                '--key', key, '--out', output,
            ], check=True)
            subprocess.run([
                sys.executable, ROOT / 'tools/verify_release.py',
                '--manifest', output / f'{board}.manifest.json', '--image', output / f'{board}.bin',
            ], check=True)
    # Keep the embedded manifest block consumed by the controller web interface.
    signed_notes = (output / 'release-notes.md').read_text()
    notes += '- The signed applications and manifests can be installed through OTA.\n'
    return notes + '\n<!-- ldg-manifests\n' + signed_notes.split('<!-- ldg-manifests\n', 1)[1]


def package(tag, site, builds, output, signing_key):
    check_tag(tag)
    catalog = read_catalog((site / 'catalog.json').read_bytes(), tag)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if catalog['commit'] != commit:
        raise ValueError('Catalog was built from a different commit')

    elfs = []
    for profile in catalog['profiles']:
        elf = builds / ('build-' + profile['id']) / 'ldgcontrol.elf'
        if sha(elf.read_bytes()) != profile['build']['elf_sha256']:
            raise ValueError(f'Diagnostic ELF mismatch for {profile["id"]}')
        elfs.append(elf)

    output.mkdir(parents=True)
    write_zip(output / SITE_ARCHIVE, site, (
        path for path in site.rglob('*') if path.is_file()
        and path.relative_to(site).parts[0] not in {'firmware', SOURCE_ARCHIVE}
    ))
    write_zip(output / 'diagnostic-elf.zip', builds, elfs)
    shutil.copyfile(site / SOURCE_ARCHIVE, output / SOURCE_ARCHIVE)
    for board in BOARDS:
        shutil.copyfile(site / 'firmware' / board / 'usb-firmware.zip',
                        output / f'firmware-{board}.zip')
    (output / 'release-notes.md').write_text(release_notes(tag, site, output, signing_key))
    (output / 'SHA256SUMS').write_text(''.join(
        f'{sha(path.read_bytes())}  {path.name}\n' for path in sorted(output.iterdir())
    ))


def verified_downloads(release):
    checksums = {}
    for line in (release / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if not re.fullmatch(r'[0-9a-f]{64}', digest) or name in checksums:
            raise ValueError('Invalid release checksum list')
        checksums[name] = digest
    required = [SITE_ARCHIVE, SOURCE_ARCHIVE, *(f'firmware-{board}.zip' for board in BOARDS)]
    for name in required:
        if sha((release / name).read_bytes()) != checksums.get(name):
            raise ValueError(f'Release checksum mismatch for {name}')
    return {name: checksums[name] for name in required}


def restore(tag, release, output, allow_dirty=False):
    checksums = verified_downloads(release)
    with ZipFile(release / SITE_ARCHIVE) as archive:
        catalog_data = archive.read('catalog.json')
    catalog = read_catalog(catalog_data, tag)
    prepare_site(output, allow_dirty)
    (output / 'catalog.json').write_bytes(catalog_data)
    shutil.copyfile(release / SOURCE_ARCHIVE, output / SOURCE_ARCHIVE)
    for board in BOARDS:
        bundle = release / f'firmware-{board}.zip'
        destination = output / 'firmware' / board
        destination.mkdir(parents=True)
        with ZipFile(bundle) as archive:
            if len(archive.namelist()) != len(USB_FILES) or set(archive.namelist()) != USB_FILES:
                raise ValueError(f'Unexpected USB archive contents for {board}')
            for name in sorted(USB_FILES):
                (destination / name).write_bytes(archive.read(name))
        shutil.copyfile(bundle, destination / 'usb-firmware.zip')

    website_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    record = {'schema': 1, 'website_commit': website_commit, 'firmware_tag': tag,
              'firmware_commit': catalog['commit'], 'release_sha256': checksums}
    (output / 'site-identity.json').write_text(json.dumps(record, indent=2) + '\n')
    return catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('check-tag').add_argument('tag')
    pack = commands.add_parser('package')
    pack.add_argument('tag')
    pack.add_argument('--site', type=Path, required=True)
    pack.add_argument('--build-root', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'check-tag':
        print(check_tag(args.tag))
    else:
        package(args.tag, args.site, args.build_root, args.output,
                os.environ.pop('LDG_OTA_SIGNING_KEY', ''))
