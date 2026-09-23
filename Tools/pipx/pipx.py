#!/usr/bin/env python3
"""
PythonX pipx - third-party package installer for PythonX environments.

pipx installs PythonX itself and third-party packages into a PythonX
environment. The --os/-os flag marks an installation as intended for a
PythonX OS image and performs the package's OS-build compatibility check.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

PYPI = "https://pypi.org/pypi"
DEFAULT_HOME = Path.home() / ".pythonx"

WHEEL_RE = re.compile(
    r"^(?P<name>.+?)-(?P<version>[^-]+)-(?P<python>[^-]+)-"
    r"(?P<abi>[^-]+)-(?P<platform>[^.]+(?:\.[^.]+)*)\.whl$"
)
REQ_NAME_RE = re.compile(r"^([A-Za-z0-9_.-]+)")


def normalize(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def home() -> Path:
    return Path(os.environ.get("PYTHONX_HOME", DEFAULT_HOME)).expanduser().resolve()


def packages_dir() -> Path:
    return home() / "site-packages"


def metadata_dir() -> Path:
    return home() / "metadata"


def installed_path(name: str) -> Path:
    return packages_dir() / normalize(name)


def fetch_json(url: str) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "PythonX-pipx/1.0"},
    )
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def download(url: str, destination: Path) -> None:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "PythonX-pipx/1.0"},
    )
    with urllib.request.urlopen(req, timeout=120) as response, destination.open("wb") as out:
        shutil.copyfileobj(response, out)


def version_key(version: str) -> tuple:
    parts = re.split(r"[.+!-]", version.lower())
    result: list[Any] = []
    for part in parts:
        if part.isdigit():
            result.append((0, int(part)))
        else:
            result.append((1, part))
    return tuple(result)


def current_tags() -> list[tuple[str, str, str]]:
    major, minor = sys.version_info[:2]
    impl = "cp"
    py = f"cp{major}{minor}"
    abi = py
    system = platform.system().lower()
    machine = platform.machine().lower()

    tags = [
        (py, abi, f"{system}_{machine}"),
        (py, abi, "any"),
        (f"py{major}", "none", "any"),
        (f"py{major}{minor}", "none", "any"),
        ("py3", "none", "any"),
    ]

    if system == "linux":
        tags.insert(1, (py, abi, "manylinux_2_17_" + machine))
        tags.insert(2, (py, abi, "manylinux2014_" + machine))
        tags.insert(3, (py, abi, "linux_" + machine))
    elif system == "darwin":
        tags.insert(1, (py, abi, "macosx_11_0_" + machine))
    elif system == "windows":
        tags.insert(1, (py, abi, "win_" + machine))

    return tags


def wheel_score(filename: str) -> int:
    match = WHEEL_RE.match(filename)
    if not match:
        return -1

    py = match.group("python")
    abi = match.group("abi")
    plat = match.group("platform")

    score = -1
    for index, (wanted_py, wanted_abi, wanted_plat) in enumerate(current_tags()):
        if py == wanted_py and abi == wanted_abi and (
            plat == wanted_plat or (wanted_plat == "any" and plat == "any")
        ):
            score = 10000 - index
            break

    if score < 0 and py.startswith("py") and plat == "any":
        score = 100

    return score


def choose_distribution(data: dict[str, Any], requested: str | None) -> dict[str, Any]:
    releases = data.get("releases", {})
    if requested:
        files = releases.get(requested, [])
        if not files:
            raise RuntimeError(f"PythonX pipx: version {requested!r} was not found.")
    else:
        versions = [v for v, files in releases.items() if files]
        if not versions:
            raise RuntimeError("PythonX pipx: PyPI has no downloadable releases.")
        requested = max(versions, key=version_key)
        files = releases[requested]

    wheels = [f for f in files if f.get("packagetype") == "bdist_wheel"]
    scored = sorted(
        ((wheel_score(f["filename"]), f) for f in wheels),
        key=lambda item: item[0],
        reverse=True,
    )
    if scored and scored[0][0] >= 0:
        return scored[0][1]

    sdists = [f for f in files if f.get("packagetype") == "sdist"]
    if sdists:
        return sdists[0]

    raise RuntimeError(
        f"PythonX pipx: no compatible wheel or source distribution was found for {data.get('info', {}).get('name', requested)}."
    )


def verify_hash(path: Path, expected: str | None) -> None:
    if not expected:
        return
    algorithm, _, value = expected.partition("=")
    digest = hashlib.new(algorithm)
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != value:
        raise RuntimeError(f"PythonX pipx: checksum verification failed for {path.name}.")


def extract_archive(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            z.extractall(destination)
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as t:
            t.extractall(destination, filter="data")
    else:
        raise RuntimeError(f"PythonX pipx: unsupported distribution archive: {archive.name}")

    entries = list(destination.iterdir())
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return destination


def read_wheel_metadata(archive: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    with zipfile.ZipFile(archive) as z:
        metadata_files = [n for n in z.namelist() if n.endswith(".dist-info/METADATA")]
        if not metadata_files:
            return result
        text = z.read(metadata_files[0]).decode("utf-8", "replace")
    for line in text.splitlines():
        if ": " in line:
            key, value = line.split(": ", 1)
            if key in {"Name", "Version", "Requires-Dist"}:
                result.setdefault(key, value)
    return result


def extract_wheel(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(destination)


def package_metadata(name: str, version: str, os_build: bool) -> dict[str, Any]:
    return {
        "name": name,
        "version": version,
        "pythonx": True,
        "os_build": os_build,
        "installed_by": "pipx",
    }


def check_os_compatibility(info: dict[str, Any], filename: str, wheel_metadata: dict[str, str] | None = None) -> None:
    """
    Conservative pre-install check.

    A package can opt out of PythonX OS builds by publishing the metadata
    marker PythonX-OS-Unsupported: true. pipx also rejects distributions
    whose wheel is explicitly tied to the current host OS when an
    OS-independent distribution is unavailable.
    """
    project = info.get("info", {})
    wheel_metadata = wheel_metadata or {}
    if wheel_metadata.get("PythonX-OS-Unsupported", "").strip().lower() in {"1", "true", "yes"}:
        raise RuntimeError(
            f"PythonX pipx: {project.get("name", "package")} declares PythonX-OS support as unavailable."
        )
    classifiers = project.get("classifiers", []) or []
    description = str(project.get("description", "") or "").lower()

    marker = str(project.get("pythonx_os_unsupported", "")).lower()
    if marker in {"1", "true", "yes"}:
        raise RuntimeError(
            f"PythonX pipx: {project.get('name', 'package')} is marked as unavailable for PythonX OS builds."
        )

    host_words = (
        "requires windows",
        "requires linux",
        "requires macos",
        "requires mac os",
        "requires host operating system",
    )
    if any(word in description for word in host_words):
        raise RuntimeError(
            f"PythonX pipx: {project.get('name', 'package')} declares a host-OS requirement and cannot be installed with -os."
        )

    if filename.endswith(".whl"):
        match = WHEEL_RE.match(filename)
        if match and match.group("platform") != "any":
            # Native wheels are allowed when their runtime can be bundled;
            # the package is recorded as platform-dependent for the OS builder.
            return


def install_package(name: str, requested_version: str | None, os_build: bool) -> None:
    data = fetch_json(f"{PYPI}/{name}/json")
    project = data.get("info", {})
    canonical = project.get("name", name)
    distribution = choose_distribution(data, requested_version)

    if os_build and distribution["packagetype"] == "bdist_wheel":
        # Read package metadata before extraction so -os can reject it before installation.
        with tempfile.TemporaryDirectory(prefix="pythonx-pipx-check-") as check_tmp:
            check_archive = Path(check_tmp) / distribution["filename"]
            download(distribution["url"], check_archive)
            check_os_compatibility(data, distribution["filename"], read_wheel_metadata(check_archive))

    target = installed_path(canonical)
    target.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pythonx-pipx-") as tmp:
        tmpdir = Path(tmp)
        archive = tmpdir / distribution["filename"]
        print(f"pipx: downloading {canonical} {distribution.get('version', requested_version or '')}...")
        download(distribution["url"], archive)
        verify_hash(archive, distribution.get("digests", {}).get("sha256") and
                    f"sha256={distribution['digests']['sha256']}")

        staging = tmpdir / "staging"
        if distribution["packagetype"] == "bdist_wheel":
            extract_wheel(archive, staging)
        else:
            source = extract_archive(archive, staging)
            # pipx deliberately does not compile arbitrary source packages
            # itself. A source distribution must provide a buildable wheel
            # through the host build backend.
            build = subprocess.run(
                [sys.executable, "-m", "pip", "wheel", "--no-deps", "--no-build-isolation",
                 "--wheel-dir", str(tmpdir / "wheel"), str(source)],
                text=True,
                capture_output=True,
            )
            if build.returncode != 0:
                raise RuntimeError(
                    "PythonX pipx: source distribution requires a build backend; "
                    "wheel build failed.\n" + build.stderr.strip()
                )
            built = sorted((tmpdir / "wheel").glob("*.whl"))
            if not built:
                raise RuntimeError("PythonX pipx: build backend produced no wheel.")
            staging = tmpdir / "wheel-staging"
            extract_wheel(built[0], staging)

        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(staging, target)

    meta = package_metadata(canonical, project.get("version", requested_version or ""), os_build)
    metadata_dir().mkdir(parents=True, exist_ok=True)
    (metadata_dir() / f"{normalize(canonical)}.json").write_text(
        json.dumps(meta, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"pipx: installed {canonical} into {target}")
    if os_build:
        print("pipx: marked for PythonX OS build.")


def list_packages() -> None:
    directory = packages_dir()
    if not directory.exists():
        print("pipx: no packages installed.")
        return
    items = sorted(p for p in directory.iterdir() if p.is_dir())
    if not items:
        print("pipx: no packages installed.")
        return
    for item in items:
        meta_file = metadata_dir() / f"{item.name}.json"
        version = ""
        os_build = False
        if meta_file.exists():
            meta = json.loads(meta_file.read_text(encoding="utf-8"))
            version = str(meta.get("version", ""))
            os_build = bool(meta.get("os_build", False))
        suffix = " [OS]" if os_build else ""
        print(f"{item.name} {version}{suffix}")


def uninstall(name: str) -> None:
    target = installed_path(name)
    if not target.exists():
        raise RuntimeError(f"pipx: package {name!r} is not installed.")
    shutil.rmtree(target)
    meta = metadata_dir() / f"{normalize(name)}.json"
    meta.unlink(missing_ok=True)
    print(f"pipx: removed {name}")


def show_environment() -> None:
    print(f"PythonX home: {home()}")
    print(f"PythonX packages: {packages_dir()}")
    print(f"PythonX metadata: {metadata_dir()}")
    print(f"Python executable: {sys.executable}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pipx",
        description="Install and manage third-party packages in a PythonX environment.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    install = sub.add_parser("install", help="Install PythonX or a third-party package.")
    install.add_argument("package")
    install.add_argument("--version", "-v", dest="version")
    install.add_argument("-os", "--os", action="store_true",
                          dest="os_build",
                          help="Install for a PythonX OS build; reject host-OS-dependent packages.")

    sub.add_parser("list", help="List installed PythonX packages.")

    remove = sub.add_parser("remove", aliases=["uninstall"], help="Remove an installed package.")
    remove.add_argument("package")

    sub.add_parser("environment", aliases=["env"], help="Show the active PythonX environment.")

    args = parser.parse_args(argv)

    try:
        if args.command == "install":
            install_package(args.package, args.version, args.os_build)
        elif args.command == "list":
            list_packages()
        elif args.command in {"remove", "uninstall"}:
            uninstall(args.package)
        elif args.command in {"environment", "env"}:
            show_environment()
        return 0
    except (urllib.error.HTTPError, urllib.error.URLError) as exc:
        print(f"pipx: network error: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"pipx: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
