# SPDX-License-Identifier: GPL-3.0-or-later
# Adapted from Remote-LDGControl 0d242cc.
"""Prepare Node/npm automatically without changing global PATH or packages."""
from contextlib import contextmanager
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath


NODE_VERSION = "24.19.0"
# https://nodejs.org/download/release/v24.19.0/SHASUMS256.txt
NODE_WINDOWS_SHA256 = {
    "x64": "57f71ab3652e797d84acddc79c81cc9ff1c6ddb2a1974cdb83f00fee9bff4c73",
    "arm64": "8502f4a50b458d4cc38ed8f2001556c2cd239d464920f74017926ccb1e1c157f",
}


@contextmanager
def bootstrap_lock(project):
    """Keep simultaneous Build/Upload tasks from replacing the same tools."""
    tools = project / ".tools" / "web-tools"
    tools.mkdir(parents=True, exist_ok=True)
    with (tools / "bootstrap.lock").open("a+b") as lock:
        lock.seek(0)
        if not lock.read(1):
            lock.write(b"0")
            lock.flush()
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            deadline = time.monotonic() + 600
            while True:
                try:
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Another web-tools setup is still running; retry Build.")
                    time.sleep(0.2)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def find_node():
    node = shutil.which("node")
    if node:
        return node
    if sys.platform == "win32":
        directories = []
        for name in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
            if os.environ.get(name):
                directories.append(Path(os.environ[name]) / "nodejs")
        if os.environ.get("LOCALAPPDATA"):
            local = Path(os.environ["LOCALAPPDATA"])
            directories.extend((local / "Programs" / "nodejs", local / "nodejs"))
        if os.environ.get("NVM_SYMLINK"):
            directories.append(Path(os.environ["NVM_SYMLINK"]))
        for directory in directories:
            candidate = directory / "node.exe"
            if candidate.is_file():
                return str(candidate)
    raise RuntimeError("Node.js 18+ was not found. Install Node.js LTS with npm.")


def node_version(node, project):
    try:
        version = subprocess.run([node, "--version"], cwd=project, check=True,
                                 capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError("Cannot run Node.js at %s: %s" % (node, error)) from error
    match = re.match(r"^v(\d+)\.", version)
    if not match or int(match.group(1)) < 18:
        raise RuntimeError("Node.js 18+ is required; found %s at %s." % (version, node))
    return version


def find_npm_cli(node):
    executable = Path(node).resolve()
    candidates = [executable.parent / "node_modules/npm/bin/npm-cli.js",
                  executable.parent.parent / "lib/node_modules/npm/bin/npm-cli.js"]
    npm = shutil.which("npm")
    if npm:
        command = Path(npm).resolve()
        if command.suffix == ".js":
            candidates.append(command)
        candidates.append(command.parent / "node_modules/npm/bin/npm-cli.js")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("npm was not found. Install Node.js LTS with its bundled npm.")


def install_windows_node(project):
    machine = platform.machine().lower()
    arch = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine)
    if arch is None:
        raise RuntimeError("Automatic Node.js setup requires Windows x64 or ARM64.")
    name = "node-v%s-win-%s" % (NODE_VERSION, arch)
    tools = project / ".tools" / "web-tools"
    tools.mkdir(parents=True, exist_ok=True)
    target = tools / name
    executable = target / "node.exe"
    npm = target / "node_modules/npm/bin/npm-cli.js"
    marker = target / ".ldg-archive-sha256"
    digest = NODE_WINDOWS_SHA256[arch]
    if executable.is_file() and npm.is_file() and marker.is_file() and marker.read_text().strip() == digest:
        return str(executable)
    print("[build_web] Downloading local Node.js %s for Windows %s..." % (NODE_VERSION, arch))
    try:
        with tempfile.TemporaryDirectory(prefix="node-setup-", dir=tools) as temporary:
            staging = Path(temporary)
            archive = staging / "node.zip"
            url = "https://nodejs.org/download/release/v%s/%s.zip" % (NODE_VERSION, name)
            checksum = hashlib.sha256()
            size = 0
            with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > 64 * 1024 * 1024:
                        raise RuntimeError("Node.js archive exceeds the download size limit.")
                    checksum.update(chunk)
                    output.write(chunk)
            if checksum.hexdigest() != digest:
                raise RuntimeError("Node.js archive SHA-256 mismatch; download was not installed.")
            with zipfile.ZipFile(archive) as package:
                if sum(item.file_size for item in package.infolist()) > 256 * 1024 * 1024:
                    raise RuntimeError("Node.js archive exceeds the extraction size limit.")
                for item in package.infolist():
                    member = PurePosixPath(item.filename)
                    if (member.is_absolute() or ".." in member.parts or not member.parts
                            or member.parts[0] != name or "\\" in item.filename or ":" in item.filename):
                        raise RuntimeError("Unsafe Node.js archive member.")
                package.extractall(staging)
            extracted = staging / name
            if not (extracted / "node.exe").is_file() or not (extracted / "node_modules/npm/bin/npm-cli.js").is_file():
                raise RuntimeError("Node.js archive is missing node.exe or npm.")
            (extracted / marker.name).write_text(digest + "\n")
            if target.exists():
                shutil.rmtree(target)
            extracted.rename(target)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        raise RuntimeError("Cannot prepare local Node.js: %s. Check internet access and retry Build." % error) from error
    return str(executable)


def install_posix_node(project):
    import tarfile
    os_name={"linux":"linux","darwin":"darwin"}.get(sys.platform)
    arch={"x86_64":"x64","aarch64":"arm64","arm64":"arm64"}.get(platform.machine().lower())
    if not os_name or not arch:raise RuntimeError("Unsupported host; provide Node.js 18+.")
    name=f"node-v{NODE_VERSION}-{os_name}-{arch}"
    target=project/".tools/web-tools"/name
    binary=target/"bin/node"
    if binary.exists():return str(binary)
    base=f"https://nodejs.org/download/release/v{NODE_VERSION}/"
    archive=name+".tar.xz"
    checks=urllib.request.urlopen(base+"SHASUMS256.txt",timeout=30).read(65536).decode()
    expected=next(line.split()[0] for line in checks.splitlines() if line.split()[-1]==archive)
    target.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as directory:
        package=Path(directory)/archive
        digest=hashlib.sha256();size=0
        with urllib.request.urlopen(base+archive,timeout=60) as source,package.open("wb") as dest:
            while True:
                chunk=source.read(65536)
                if not chunk:break
                size+=len(chunk)
                if size>150*1024*1024:raise RuntimeError("Node archive size limit")
                digest.update(chunk);dest.write(chunk)
        if digest.hexdigest()!=expected:raise RuntimeError("Node SHA-256 mismatch")
        with tarfile.open(package) as tar:
            # Python 3.12+ data filter checks paths, symlinks and special files.
            tar.extractall(directory,filter="data")
        (Path(directory)/name).rename(target)
    return str(binary)


def ensure_tools(project):
    try:
        node=find_node()
        return node,node_version(node,project),find_npm_cli(node)
    except RuntimeError:
        node=install_windows_node(project) if sys.platform=="win32" else install_posix_node(project)
        return node,node_version(node,project),find_npm_cli(node)
