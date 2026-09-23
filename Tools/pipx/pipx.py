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
import sysconfig
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import urllib.parse
import zipfile
from email.parser import Parser
from pathlib import Path
from typing import Any

PYPI = "https://pypi.org/pypi"
PYPI_SIMPLE = "https://pypi.org/simple/"
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


def pythonx_prefix() -> Path:
    """Return the PythonX environment that pipx is installing into."""
    configured = os.environ.get("PYTHONX_PREFIX")
    if configured:
        return Path(configured).expanduser().resolve()
    return Path(sys.prefix).resolve()


def packages_dir() -> Path:
    """Return the live site-packages directory used by this PythonX."""
    configured = os.environ.get("PYTHONX_SITE_PACKAGES")
    if configured:
        return Path(configured).expanduser().resolve()
    try:
        purelib = sysconfig.get_path("purelib")
    except Exception:
        purelib = None
    if purelib:
        return Path(purelib).resolve()
    return pythonx_prefix() / "Lib" / "site-packages"


def data_home() -> Path:
    return home()




def metadata_dir() -> Path:
    return data_home() / "metadata"


def installed_path(name: str) -> Path:
    return packages_dir() / normalize(name)


def fetch_json(url: str) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "PythonX-pipx/1.0"},
    )
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def fetch_text(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "PythonX-pipx/1.0"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return response.read().decode("utf-8", "replace")


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
        candidates = [(requested, releases.get(requested, []))]
    else:
        candidates = sorted(
            ((v, f) for v, f in releases.items() if f),
            key=lambda x: version_key(x[0]),
            reverse=True,
        )
    for version, files in candidates:
        wheels = [f for f in files if f.get("packagetype") == "bdist_wheel"]
        scored = sorted(
            ((wheel_score(f.get("filename", "")), f) for f in wheels),
            key=lambda x: x[0], reverse=True,
        )
        if scored and scored[0][0] >= 0:
            return scored[0][1]
        sdists = [f for f in files if f.get("packagetype") == "sdist"]
        if sdists:
            return sdists[0]
    raise RuntimeError(f"PythonX pipx: no compatible distribution found for {data.get('info', {}).get('name', requested)}.")


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


def read_wheel_metadata(archive: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"Requires-Dist": []}
    with zipfile.ZipFile(archive) as z:
        names = [n for n in z.namelist() if n.endswith(".dist-info/METADATA")]
        if not names:
            return result
        message = Parser().parsestr(z.read(names[0]).decode("utf-8", "replace"))
    for key in ("Name", "Version", "Summary", "PythonX-OS-Unsupported"):
        value = message.get(key)
        if value is not None:
            result[key] = value
    result["Requires-Dist"] = message.get_all("Requires-Dist", [])
    return result


def extract_wheel(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(destination)


def package_metadata(name: str, version: str, os_build: bool, **extra: Any) -> dict[str, Any]:
    return {
        "name": name,
        "version": version,
        "pythonx": True,
        "environment": str(pythonx_prefix()),
        "os_build": os_build,
        "installed_by": "pipx",
        **extra,
    }


def metadata_path(name: str) -> Path:
    return metadata_dir() / f"{normalize(name)}.json"


def load_metadata(name: str) -> dict[str, Any]:
    path = metadata_path(name)
    if not path.exists():
        raise RuntimeError(f"PythonX pipx: package {name!r} is not installed.")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"PythonX pipx: invalid metadata for {name!r}: {exc}") from exc


def installed_names() -> list[str]:
    if not metadata_dir().exists():
        return []
    return sorted(
        p.stem for p in metadata_dir().glob("*.json")
        if p.is_file()
    )


def check_os_compatibility(info: dict[str, Any], filename: str, wheel_metadata: dict[str, str] | None = None) -> None:
    """Reject a package explicitly marked as unsuitable for PythonX OS builds."""
    project = info.get("info", {})
    wheel_metadata = wheel_metadata or {}
    if wheel_metadata.get("PythonX-OS-Unsupported", "").strip().lower() in {"1", "true", "yes"}:
        raise RuntimeError(
            f"PythonX pipx: {project.get('name', 'package')} declares PythonX-OS support as unavailable."
        )

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
            f"PythonX pipx: {project.get('name', 'package')} declares a host-OS requirement "
            "and cannot be installed with -os."
        )


def copy_tree_contents(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        target = destination / item.name
        if item.is_dir():
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(item, target)
        else:
            shutil.copy2(item, target)



def package_json(name: str, index_url: str | None = None) -> dict[str, Any]:
    if not index_url or index_url.rstrip("/") == PYPI_SIMPLE.rstrip("/"):
        return fetch_json(f"{PYPI}/{urllib.parse.quote(name, safe='')}/json")
    base = index_url.rstrip("/")
    if base.endswith("/simple"):
        base = base[:-7]
    return fetch_json(base + f"/pypi/{urllib.parse.quote(name, safe='')}/json")


def simple_project_files(name: str, index_url: str) -> list[dict[str, Any]]:
    url = index_url.rstrip("/") + "/" + urllib.parse.quote(normalize(name), safe="") + "/"
    html = fetch_text(url)
    links = re.findall('<a[^>]+href=["\\\']([^"\\\']+)["\\\']', html, re.I)
    files = []
    for href in links:
        file_url = urllib.parse.urljoin(url, href)
        filename = urllib.parse.unquote(file_url.split("#", 1)[0].rsplit("/", 1)[-1])
        if filename.endswith(".whl"):
            files.append({"filename": filename, "url": file_url, "packagetype": "bdist_wheel", "digests": {}})
        elif filename.endswith((".tar.gz", ".zip")):
            files.append({"filename": filename, "url": file_url, "packagetype": "sdist", "digests": {}})
    return files


def package_data(name: str, indexes_list: list[str]) -> tuple[dict[str, Any], str]:
    errors = []
    for index in indexes_list:
        try:
            return package_json(name, index), index
        except Exception as exc:
            try:
                files = simple_project_files(name, index)
                if files:
                    return {"info": {"name": name}, "releases": {"0": files}}, index
            except Exception as simple_exc:
                errors.append(f"{index}: {exc}; simple: {simple_exc}")
            else:
                errors.append(f"{index}: {exc}")
    raise RuntimeError(f"PythonX pipx: package {name!r} was not found in configured indexes. " + " | ".join(errors))




def requirement_parts(requirement: str) -> tuple[str, list[tuple[str, str]]]:
    requirement = requirement.split(";", 1)[0].strip()
    match = re.match(r"^([A-Za-z0-9_.-]+)\s*(.*)$", requirement)
    if not match:
        raise RuntimeError(f"PythonX pipx: unsupported dependency: {requirement}")
    name, spec = match.groups()
    return name, re.findall(r"(===|==|!=|>=|<=|>|<|~=)\s*([A-Za-z0-9_.+!-]+)", spec)


def version_satisfies(version: str, constraints: list[tuple[str, str]]) -> bool:
    for op, wanted in constraints:
        a, b = version_key(version), version_key(wanted)
        if op == "==" and a != b: return False
        if op == "===" and version != wanted: return False
        if op == "!=" and a == b: return False
        if op == ">=" and a < b: return False
        if op == "<=" and a > b: return False
        if op == ">" and a <= b: return False
        if op == "<" and a >= b: return False
    return True


def install_package(name: str, requested_version: str | None, os_build: bool, indexes_list: list[str] | None = None, no_deps: bool = False, seen: set[str] | None = None) -> None:
    indexes_list = indexes_list or [PYPI_SIMPLE]
    seen = seen or set()
    if normalize(name) in seen:
        return
    seen.add(normalize(name))
    data, source_index = package_data(name, indexes_list)
    project = data.get("info", {})
    canonical = project.get("name", name)
    distribution = choose_distribution(data, requested_version)

    with tempfile.TemporaryDirectory(prefix="pythonx-pipx-") as tmp:
        tmpdir = Path(tmp)
        archive = tmpdir / distribution["filename"]

        print(
            f"pipx: downloading {canonical} "
            f"{distribution.get('version', requested_version or '')}..."
        )
        download(distribution["url"], archive)
        verify_hash(
            archive,
            distribution.get("digests", {}).get("sha256")
            and f"sha256={distribution['digests']['sha256']}",
        )

        if os_build and distribution["packagetype"] == "bdist_wheel":
            check_os_compatibility(
                data,
                distribution["filename"],
                read_wheel_metadata(archive),
            )

        staging = tmpdir / "staging"
        if distribution["packagetype"] == "bdist_wheel":
            extract_wheel(archive, staging)
        else:
            source = extract_archive(archive, staging)
            build = subprocess.run(
                [
                    sys.executable, "-m", "pip", "wheel",
                    "--no-deps", "--no-build-isolation",
                    "--wheel-dir", str(tmpdir / "wheel"), str(source),
                ],
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

        target = packages_dir()
        target.mkdir(parents=True, exist_ok=True)

        # Install the complete wheel contents into PythonX's real
        # site-packages. A distribution may contain multiple top-level
        # packages, namespace fragments, .pth files, and dist-info.
        old_meta_path = metadata_path(canonical)
        if old_meta_path.exists():
            try:
                old_meta = json.loads(old_meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                old_meta = {}
            for relative in old_meta.get("installed_files", []):
                old_file = target / relative
                if old_file.is_file() or old_file.is_symlink():
                    old_file.unlink()

        installed_files: list[str] = []
        for source in staging.rglob("*"):
            relative = source.relative_to(staging)
            destination = target / relative
            if source.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists() or destination.is_symlink():
                if destination.is_dir() and not destination.is_symlink():
                    shutil.rmtree(destination)
                else:
                    destination.unlink()
            shutil.copy2(source, destination)
            installed_files.append(relative.as_posix())

    metadata_dir().mkdir(parents=True, exist_ok=True)
    meta = package_metadata(
        canonical,
        project.get("version", requested_version or ""),
        os_build,
        summary=project.get("summary", ""),
        requires_dist=project.get("requires_dist", []) or [],
        distribution=distribution["filename"],
        source_index=source_index,
        installed_files=installed_files,
    )
    metadata_path(canonical).write_text(
        json.dumps(meta, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    if not no_deps:
        for requirement in meta.get("requires_dist", []):
            dep_name, constraints = requirement_parts(requirement)
            dep_meta_file = metadata_path(dep_name)
            if dep_meta_file.exists():
                installed = load_metadata(dep_name)
                if version_satisfies(installed.get("version", ""), constraints):
                    continue
            install_package(dep_name, None, os_build, indexes_list, False, seen)

    print(f"pipx: installed {canonical} into PythonX environment: {target}")
    if os_build:
        print("pipx: marked for PythonX OS build.")


def update_package(name: str, requested_version: str | None, os_build: bool | None, indexes_list: list[str] | None = None, no_deps: bool = False) -> None:
    old = load_metadata(name)
    use_os = bool(old.get("os_build", False)) if os_build is None else os_build
    print(f"pipx: updating {old['name']}...")
    install_package(old["name"], requested_version, use_os, indexes_list, no_deps)


def reinstall_package(name: str) -> None:
    meta = load_metadata(name)
    install_package(meta["name"], meta.get("version"), bool(meta.get("os_build", False)))


def uninstall(name: str) -> None:
    meta = load_metadata(name)
    target = packages_dir()
    for relative in meta.get("installed_files", []):
        path = target / relative
        if path.is_file() or path.is_symlink():
            path.unlink()
    for relative in sorted(meta.get("installed_files", []), reverse=True):
        parent = (target / relative).parent
        while parent != target and parent.exists():
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent
    metadata_path(meta["name"]).unlink(missing_ok=True)
    print(f"pipx: removed {meta['name']} from PythonX")


def edit_package(name: str) -> None:
    meta = load_metadata(name)
    target = installed_path(meta["name"])
    editor = os.environ.get("EDITOR") or os.environ.get("VISUAL")
    if not editor:
        editor = "notepad" if os.name == "nt" else "nano"
    try:
        subprocess.run([editor, str(target)], check=False)
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"PythonX pipx: editor {editor!r} was not found. "
            "Set EDITOR or VISUAL to your preferred editor."
        ) from exc


def show_package(name: str) -> None:
    meta = load_metadata(name)
    print(json.dumps(meta, indent=2, sort_keys=True))


def files_package(name: str) -> None:
    meta = load_metadata(name)
    target = packages_dir()
    files = meta.get("installed_files", [])
    if not files:
        raise RuntimeError(f"PythonX pipx: no installed-file manifest exists for {name!r}.")
    for relative in sorted(files):
        path = target / relative
        if path.is_file():
            print(relative)


def verify_package(name: str) -> None:
    meta = load_metadata(name)
    target = packages_dir()
    files = meta.get("installed_files", [])
    missing = [relative for relative in files if not (target / relative).is_file()]
    if missing:
        raise RuntimeError(
            f"PythonX pipx: {name!r} is incomplete; {len(missing)} installed files are missing."
        )
    py_files = [relative for relative in files if relative.endswith(".py")]
    print(f"pipx: {meta['name']} {meta.get('version', '')} is installed in PythonX.")
    print(f"pipx: {len(py_files)} Python source files found.")
    print(f"pipx: OS build package: {'yes' if meta.get('os_build') else 'no'}")


def clear_packages() -> None:
    if packages_dir().exists():
        shutil.rmtree(packages_dir())
    if metadata_dir().exists():
        shutil.rmtree(metadata_dir())
    print("pipx: cleared the PythonX third-party package environment.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pipx",
        description="Install and manage third-party packages in a PythonX environment.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    install = sub.add_parser("install", help="Install PythonX or a third-party package.")
    install.add_argument("package")
    install.add_argument("--version", "-v", dest="version")
    install.add_argument("--index-url", dest="index_url")
    install.add_argument("--extra-index-url", action="append", default=[])
    install.add_argument("--no-deps", action="store_true")
    install.add_argument(
        "-os", "--os", action="store_true", dest="os_build",
        help="Install for a PythonX OS build; reject host-OS-dependent packages.",
    )

    update = sub.add_parser("update", aliases=["upgrade"], help="Update an installed package.")
    update.add_argument("package", nargs="?")
    update.add_argument("--version", "-v", dest="version")
    update.add_argument("--index-url", dest="index_url")
    update.add_argument("--extra-index-url", action="append", default=[])
    update.add_argument("--no-deps", action="store_true")
    update.add_argument("-os", "--os", action="store_true", dest="os_build")
    update.add_argument("--no-os", action="store_false", dest="os_build")
    update.add_argument("--all", action="store_true", dest="all_packages")
    update.set_defaults(os_build=None)

    reinstall = sub.add_parser("reinstall", help="Reinstall an installed package.")
    reinstall.add_argument("package")

    sub.add_parser("list", aliases=["ls"], help="List installed packages.")

    remove = sub.add_parser("remove", aliases=["uninstall", "rm"], help="Uninstall a package.")
    remove.add_argument("package")

    edit = sub.add_parser("edit", help="Open an installed package in the configured editor.")
    edit.add_argument("package")

    show = sub.add_parser("show", aliases=["info"], help="Show package metadata.")
    show.add_argument("package")

    files = sub.add_parser("files", help="List files installed by a package.")
    files.add_argument("package")

    verify = sub.add_parser("verify", help="Verify that a package installation is present.")
    verify.add_argument("package")

    sub.add_parser("environment", aliases=["env"], help="Show the active PythonX environment.")
    sub.add_parser("clear", help="Remove all third-party packages from the environment.")

    args = parser.parse_args(argv)

    try:
        if args.command == "install":
            install_package(args.package, args.version, args.os_build, [args.index_url] + args.extra_index_url if args.index_url else args.extra_index_url, args.no_deps)
        elif args.command in {"update", "upgrade"}:
            indexes = ([args.index_url] if args.index_url else []) + args.extra_index_url
            if args.all_packages:
                for package in installed_names():
                    update_package(package, None, None, indexes or None, args.no_deps)
            elif args.package:
                update_package(args.package, args.version, args.os_build, indexes or None, args.no_deps)
            else:
                raise RuntimeError("pipx: update requires <package> or --all.")
        elif args.command == "reinstall":
            reinstall_package(args.package)
        elif args.command in {"list", "ls"}:
            list_packages()
        elif args.command in {"remove", "uninstall", "rm"}:
            uninstall(args.package)
        elif args.command == "edit":
            edit_package(args.package)
        elif args.command in {"show", "info"}:
            show_package(args.package)
        elif args.command == "files":
            files_package(args.package)
        elif args.command == "verify":
            verify_package(args.package)
        elif args.command in {"environment", "env"}:
            show_environment()
        elif args.command == "clear":
            clear_packages()
        return 0
    except (urllib.error.HTTPError, urllib.error.URLError) as exc:
        print(f"pipx: network error: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"pipx: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
