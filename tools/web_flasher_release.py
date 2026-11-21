#!/usr/bin/env python3
"""Package a tagged USB release and restore its firmware for a Pages update."""
import argparse
from io import BytesIO
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
USB_ARCHIVE = 'firmware-usb.zip'
ELF_ARCHIVE = 'diagnostic-elf.zip'
USB_MEMBERS = {board: f'firmware/{board}/usb-firmware.zip' for board in BOARDS}


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


def release_notes(tag):
    return (f'LDGControl-IDF {tag}\n\n'
             '[Open the USB installer](https://ra3vld.github.io/LDGControl-IDF/).\n\n'
             '- `firmware-usb.zip` contains USB bundles for all boards, the catalog and sources.\n'
             '- `diagnostic-elf.zip` contains the matching ELF files.\n'
             '- `SHA256SUMS` verifies the release files.\n')


def write_release_metadata(output, notes, assets):
    (output / 'release-notes.md').write_text(notes)
    (output / 'SHA256SUMS').write_text(''.join(
        f'{sha((output / name).read_bytes())}  {name}\n' for name in sorted(assets)
    ))


def sign(tag, output, key):
    catalog, _, _, bundles, checksums = release_payload(tag, output, verify_diagnostics=True)
    if USB_ARCHIVE not in checksums:
        raise ValueError('Signing requires a compact USB release package')
    notes = release_notes(tag)
    with tempfile.TemporaryDirectory() as temp:
        scratch = Path(temp)
        for profile in catalog['profiles']:
            board = profile['id']
            with ZipFile(BytesIO(bundles[board])) as bundle:
                image = scratch / f'{board}.bin'
                image.write_bytes(bundle.read('ldgcontrol.bin'))
            identity = re.findall(rb'LDG_METADATA_V1:([^\n\x00]+)\n', image.read_bytes())
            meta = json.loads(identity[0]) if len(identity) == 1 else {}
            expected = {key: profile['build'][key] for key in ('commit', 'version', 'source_digest')}
            if any(meta.get(key) != value for key, value in {**expected, 'board': board}.items()):
                raise ValueError(f'USB package application profile mismatch for {board}')
            subprocess.run([
                sys.executable, ROOT / 'tools/package_release.py', '--board', board,
                '--version', tag_version(tag), '--image', image, '--key', key, '--out', scratch / 'ota',
            ], check=True)
            subprocess.run([
                sys.executable, ROOT / 'tools/verify_release.py',
                '--manifest', scratch / 'ota' / f'{board}.manifest.json',
                '--image', scratch / 'ota' / f'{board}.bin',
            ], check=True)
        assets = [USB_ARCHIVE, ELF_ARCHIVE]
        for board in BOARDS:
            for name in (f'{board}.bin', f'{board}.manifest.json'):
                shutil.copyfile(scratch / 'ota' / name, output / name)
                assets.append(name)
        signed_notes = (scratch / 'ota' / 'release-notes.md').read_text()
    notes += '- The signed applications and manifests can be installed through OTA.\n'
    write_release_metadata(output, notes + '\n<!-- ldg-manifests\n' +
                           signed_notes.split('<!-- ldg-manifests\n', 1)[1], assets)


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
    write_zip(output / USB_ARCHIVE, site, (
        site / name for name in ('catalog.json', SOURCE_ARCHIVE, *USB_MEMBERS.values())
    ))
    write_zip(output / ELF_ARCHIVE, builds, elfs)
    write_release_metadata(output, release_notes(tag), [USB_ARCHIVE, ELF_ARCHIVE])
    if signing_key:
        with tempfile.TemporaryDirectory() as temp:
            key = Path(temp) / 'key.pem'
            key.write_text(signing_key)
            sign(tag, output, key)


def verified_downloads(release, verify_diagnostics=False):
    checksums = {}
    for line in (release / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if not re.fullmatch(r'[0-9a-f]{64}', digest) or name in checksums:
            raise ValueError('Invalid release checksum list')
        checksums[name] = digest
    required = ([USB_ARCHIVE] if USB_ARCHIVE in checksums else
                [SITE_ARCHIVE, SOURCE_ARCHIVE, *(f'firmware-{board}.zip' for board in BOARDS)])
    if verify_diagnostics:
        required.append(ELF_ARCHIVE)
    for name in required:
        if sha((release / name).read_bytes()) != checksums.get(name):
            raise ValueError(f'Release checksum mismatch for {name}')
    return {name: checksums[name] for name in required}


def release_payload(tag, release, verify_diagnostics=False):
    checksums = verified_downloads(release, verify_diagnostics)
    if USB_ARCHIVE in checksums:
        with ZipFile(release / USB_ARCHIVE) as archive:
            names = ['catalog.json', SOURCE_ARCHIVE, *USB_MEMBERS.values()]
            if sorted(archive.namelist()) != sorted(names):
                raise ValueError('Unexpected USB release archive contents')
            catalog_data = archive.read('catalog.json')
            sources = archive.read(SOURCE_ARCHIVE)
            bundles = {board: archive.read(name) for board, name in USB_MEMBERS.items()}
    else:
        with ZipFile(release / SITE_ARCHIVE) as archive:
            catalog_data = archive.read('catalog.json')
        sources = (release / SOURCE_ARCHIVE).read_bytes()
        bundles = {board: (release / f'firmware-{board}.zip').read_bytes() for board in BOARDS}
    return read_catalog(catalog_data, tag), catalog_data, sources, bundles, checksums


def restore(tag, release, output, allow_dirty=False):
    catalog, catalog_data, sources, bundles, checksums = release_payload(tag, release)
    prepare_site(output, allow_dirty)
    (output / 'catalog.json').write_bytes(catalog_data)
    (output / SOURCE_ARCHIVE).write_bytes(sources)
    for board in BOARDS:
        bundle = bundles[board]
        destination = output / 'firmware' / board
        destination.mkdir(parents=True)
        with ZipFile(BytesIO(bundle)) as archive:
            if len(archive.namelist()) != len(USB_FILES) or set(archive.namelist()) != USB_FILES:
                raise ValueError(f'Unexpected USB archive contents for {board}')
            for name in sorted(USB_FILES):
                (destination / name).write_bytes(archive.read(name))
        (destination / 'usb-firmware.zip').write_bytes(bundle)

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
    signed = commands.add_parser('sign')
    signed.add_argument('tag')
    signed.add_argument('--release-dir', type=Path, required=True)
    signed.add_argument('--key', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'check-tag':
        print(check_tag(args.tag))
    elif args.command == 'package':
        package(args.tag, args.site, args.build_root, args.output,
                os.environ.pop('LDG_OTA_SIGNING_KEY', ''))
    else:
        check_tag(args.tag)
        sign(args.tag, args.release_dir, args.key)
