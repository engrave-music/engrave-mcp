"""Assemble a relocatable Linux package around a trusted native ELF build.

Only the dynamic loader dependencies visible through ldd are bundled. Libraries
opened later with dlopen are out of scope, and validation is limited to the host
that assembled the package rather than every Linux distribution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE_ENV = ROOT / ".pixi/envs/native"
SOUNDFONT = ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"
AUDIO_THIRDPARTY = ROOT / "vendor/musescore/src/framework/audio/thirdparty"
# License texts of the codecs and synthesizer bundled into the binary. Paths are
# relative to AUDIO_THIRDPARTY and are copied with a thirdparty-<name>- prefix so
# same-named COPYING files cannot overwrite each other.
THIRDPARTY_LICENSES = (
    ("fdk-aac", "fdk-aac-2.0.3/NOTICE"),
    ("flac", "flac-1.4.3/COPYING.FDL"),
    ("flac", "flac-1.4.3/COPYING.GPL"),
    ("flac", "flac-1.4.3/COPYING.LGPL"),
    ("flac", "flac-1.4.3/COPYING.Xiph"),
    ("fluidsynth", "fluidsynth-2.3.3/LICENSE"),
    ("lame", "COPYING"),
    ("lame", "LICENSE"),
    ("opus", "opus-1.5.2/COPYING"),
    ("opusenc", "libopusenc-0.2.1/COPYING"),
)

HOST_RUNTIME = (
    re.compile(r"^(?:libc|libm|libmvec|libpthread|libdl|librt|libresolv|libutil|libanl|libBrokenLocale|libthread_db|libc_malloc_debug)\.so(?:\.|$)"),
    re.compile(r"^libnss_.*\.so(?:\.|$)"),
    re.compile(r"^libnsl\.so\.1$"),
    re.compile(r"^ld-linux.*\.so(?:\.|$)"),
    re.compile(r"^linux-vdso\.so(?:\.|$)"),
)
QT_LIBRARY = re.compile(r"^libQt[56]", re.IGNORECASE)
LDD_LINE = re.compile(
    r"^\s*(?P<name>\S+)(?:\s+=>\s+(?P<path>.*?))?(?:\s+\(0[xX][0-9a-fA-F]+\))?\s*$"
)
CLEAN_LDD_ENV = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def same_file(first, second):
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False


def is_host_runtime(soname):
    return any(pattern.match(soname) for pattern in HOST_RUNTIME)


def is_qt(soname):
    return bool(QT_LIBRARY.match(Path(soname).name))


def parse_ldd(text, source):
    if "not a dynamic executable" in text or "statically linked" in text:
        raise RuntimeError(f"{source} is not a dynamically linked ELF; no ldd closure can be packaged")
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if "not found" in line:
            name = line.split("=>", 1)[0].strip() or line
            raise RuntimeError(f"Unresolved dependency from {source}: {name}")
        match = LDD_LINE.fullmatch(line)
        if not match:
            raise RuntimeError(f"Unexpected ldd output for {source}: {line!r}")
        soname = Path(match.group("name")).name
        path = (match.group("path") or "").strip() or None
        if soname in {"", ".", ".."}:
            raise RuntimeError(f"Unsafe dependency name in ldd output for {source}: {soname!r}")
        entries.append((soname, path))
    if not entries:
        raise RuntimeError(f"ldd reported no dependencies for {source}")
    return entries


def ldd_entries(ldd, target, environment=None):
    result = subprocess.run(
        [ldd, "-r", str(target)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
    )
    if result.returncode or "not found" in result.stderr or "undefined symbol" in result.stderr:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"ldd failed for {target} (exit {result.returncode}): {detail}")
    return parse_ldd(result.stdout, target)


def classify_dependencies(entries):
    bundled = {}
    for soname, path in entries:
        if is_qt(soname):
            raise RuntimeError(f"Qt dependency is not allowed in the Linux package: {soname}")
        if is_host_runtime(soname):
            continue
        if path is None:
            raise RuntimeError(f"Dependency {soname} has no resolved path")
        try:
            resolved = Path(path).resolve(strict=True)
        except FileNotFoundError as error:
            raise RuntimeError(f"Dependency {soname} resolved to a missing file: {path}") from error
        if not resolved.is_file():
            raise RuntimeError(f"Dependency {soname} does not resolve to a regular file: {resolved}")
        if not resolved.is_relative_to(NATIVE_ENV.resolve()):
            raise RuntimeError(f"Non-glibc dependency is outside the locked native environment: {resolved}")
        if is_qt(resolved.name):
            raise RuntimeError(f"Qt dependency is not allowed in the Linux package: {resolved.name}")
        if soname in bundled and not same_file(bundled[soname], resolved):
            raise RuntimeError(
                f"Two different files resolve to SONAME {soname}: {bundled[soname]} and {resolved}"
            )
        bundled.setdefault(soname, resolved)
    return bundled


def copy_checked(source, destination):
    source = source.resolve(strict=True)
    if not source.is_file():
        raise RuntimeError(f"Not a regular file: {source}")
    if os.path.lexists(destination):
        raise RuntimeError(f"Refusing to overwrite package file: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def set_rpath(patchelf, target, value, output):
    target = target.resolve(strict=True)
    output = output.resolve()
    if not target.is_relative_to(output):
        raise RuntimeError(f"Refusing to modify a source file outside the package: {target}")
    target.chmod(target.stat().st_mode | stat.S_IWUSR)
    result = subprocess.run(
        [patchelf, "--set-rpath", value, str(target)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode:
        raise RuntimeError(f"patchelf failed for {target}: {result.stderr.strip()}")
    check = subprocess.run(
        [patchelf, "--print-rpath", str(target)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check.returncode or check.stdout.strip() != value:
        raise RuntimeError(f"patchelf did not set rpath {value!r} on {target}")


def validate_package(output, ldd, bundled):
    root = output.resolve()
    executable = root / "musescore-mcp"
    entries = ldd_entries(ldd, executable, CLEAN_LDD_ENV)
    resolved = {}
    for soname, path in entries:
        if is_qt(soname):
            raise RuntimeError(f"Qt dependency is not allowed in the Linux package: {soname}")
        if is_host_runtime(soname):
            continue
        if path is None:
            raise RuntimeError(f"Packaged dependency {soname} has no resolved path")
        dependency = Path(path).resolve(strict=True)
        if is_qt(dependency.name):
            raise RuntimeError(f"Qt dependency is not allowed in the Linux package: {dependency.name}")
        if not dependency.is_relative_to(root):
            raise RuntimeError(
                f"Non-glibc dependency {soname} resolved outside the package: {dependency}"
            )
        resolved[soname] = str(dependency.relative_to(root))
    if set(resolved) != set(bundled):
        missing = sorted(set(bundled) - set(resolved))
        unexpected = sorted(set(resolved) - set(bundled))
        raise RuntimeError(
            f"Packaged dependency closure differs from the source closure; missing={missing}, unexpected={unexpected}"
        )
    return resolved


def glibc_metadata(ldd, entries):
    name, version = os.confstr("CS_GNU_LIBC_VERSION").split()
    result = subprocess.run(
        [ldd, "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=CLEAN_LDD_ENV,
    )
    version_output = result.stdout or result.stderr
    lines = version_output.splitlines()
    ldd_version = lines[0].strip() if lines else ""
    if not name:
        name = "glibc" if ("glibc" in ldd_version.lower() or "gnu libc" in ldd_version.lower()) else ""
    if not version:
        match = re.search(r"(\d+\.\d+(?:\.\d+)?)", ldd_version)
        if match:
            version = match.group(1)
    if name.lower() != "glibc" or not version:
        raise RuntimeError(f"Could not determine the host glibc baseline (libc={name!r}, version={version!r})")
    return {
        "name": name,
        "version": version,
        "ldd_version": ldd_version,
        "libc_path": next((path for soname, path in entries if soname == "libc.so.6"), None),
        "architecture": platform.machine(),
        "platform": sys.platform,
        "scope": "Host glibc at package time; not a minimum-version declaration or compatibility guarantee.",
    }


def main():
    if sys.platform != "linux":
        raise SystemExit("Linux packaging must run on Linux")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/linux-package")
    parser.add_argument("--binary", type=Path, help="Override the built executable")
    args = parser.parse_args()

    output = args.output.resolve()
    binary = (args.binary or ROOT / "build/native-linux/musescore-mcp").resolve()
    assets = (ROOT / ".runtime/assets").resolve()
    upstream = ROOT / "vendor/musescore"

    if output.is_relative_to(assets):
        raise SystemExit("Package output must not be inside .runtime/assets")
    if os.path.lexists(output):
        if not output.is_dir() or any(output.iterdir()):
            raise SystemExit("Package output must be empty; choose a new --output directory")
    if not binary.is_file():
        raise RuntimeError(f"Missing native Linux executable: {binary}")
    with binary.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise RuntimeError(f"Not an ELF executable: {binary}")
    if not assets.is_dir():
        raise RuntimeError(f"Missing runtime assets: {assets}")
    soundfont = None
    if SOUNDFONT.exists():
        if not SOUNDFONT.is_file():
            raise RuntimeError(f"SoundFont is not a regular file: {SOUNDFONT}")
        soundfont = SOUNDFONT
    licenses = sorted(source for source in upstream.glob("LICENSE*") if source.is_file())
    if not licenses:
        raise RuntimeError(f"No upstream LICENSE file found under {upstream}")
    thirdparty_licenses = []
    for name, relative in THIRDPARTY_LICENSES:
        source = AUDIO_THIRDPARTY / name / relative
        if not source.is_file():
            raise RuntimeError(f"Missing bundled license text: {source}")
        thirdparty_licenses.append((source, f"thirdparty-{name}-{source.name}"))
    conda_meta = NATIVE_ENV / "conda-meta"
    if not conda_meta.is_dir():
        raise RuntimeError(f"Missing active native environment metadata: {conda_meta}")
    provenance = sorted(source for source in conda_meta.glob("*.json") if source.is_file())
    if not provenance:
        raise RuntimeError(f"No conda-meta JSON provenance found under {conda_meta}")

    ldd = "/usr/bin/ldd"
    patchelf = str(NATIVE_ENV / "bin/patchelf")
    if not Path(ldd).is_file():
        raise RuntimeError("ldd is required")
    if not Path(patchelf).is_file():
        raise RuntimeError("patchelf is required")

    # ldd runs the loader in trace mode, so this is the resolved transitive
    # closure of the original trusted binary, not just its direct DT_NEEDED list.
    interpreter = subprocess.check_output([patchelf, "--print-interpreter", str(binary)], text=True).strip()
    if interpreter != "/lib64/ld-linux-x86-64.so.2":
        raise RuntimeError(f"Unexpected executable interpreter: {interpreter}")
    entries = ldd_entries(ldd, binary, CLEAN_LDD_ENV)
    bundled = classify_dependencies(entries)
    glibc = glibc_metadata(ldd, entries)

    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit("Package output must be empty; choose a new --output directory")

    copy_checked(binary, output / "musescore-mcp")
    (output / "lib").mkdir()
    for soname in sorted(bundled):
        copy_checked(bundled[soname], output / "lib" / soname)

    set_rpath(patchelf, output / "musescore-mcp", "$ORIGIN/lib", output)
    for soname in sorted(bundled):
        set_rpath(patchelf, output / "lib" / soname, "$ORIGIN", output)

    shutil.copytree(assets, output / "assets")
    if soundfont:
        copy_checked(soundfont, output / "soundfonts/FluidR3Mono_GM.sf3")

    license_directory = output / "licenses"
    license_directory.mkdir()
    for source in licenses:
        copy_checked(source, license_directory / source.name)
    for source, name in thirdparty_licenses:
        copy_checked(source, license_directory / name)
    for source in provenance:
        copy_checked(source, license_directory / source.name)

    packaged = validate_package(output, ldd, bundled)
    files = {
        path.relative_to(output).as_posix(): {
            "bytes": path.stat().st_size,
            "sha256": digest(path),
        }
        for path in sorted(output.rglob("*"))
        if path.is_file()
    }
    total_bytes = sum(item["bytes"] for item in files.values())
    manifest = {
        "files": files,
        "total_bytes": total_bytes,
        "glibc_baseline": glibc,
        "interpreter": interpreter,
        "build_path_strings": [path.relative_to(output).as_posix()
                               for path in [output / "musescore-mcp", *sorted((output / "lib").iterdir())]
                               if str(ROOT).encode() in path.read_bytes()],
        "validation": {
            "scope": "local packaging host only",
            "runtime_smoke_test": "not run by this packaging script",
            "ldd": {
                "environment": CLEAN_LDD_ENV,
                "non_glibc_dependencies": packaged,
            },
        },
        "limitations": [
            "glibc, the dynamic loader, and the kernel VDSO are host-provided and not bundled.",
            "Only libraries reported by ldd are bundled; dlopen plugins and non-library data or configuration are not enumerated.",
            "Compatibility with other distributions, glibc versions, or architectures is not claimed.",
            "Linux assets are found beside /proc/self/exe, with argv[0] as a fallback when procfs is unavailable.",
            "The SoundFont is copied but not auto-discovered; pass --soundfont when WAV export is needed.",
            "This script does not run the packaged server or verify MCP, rendering, or audio behavior.",
        ],
        "note": "Local validation bundle. Full third-party source/license distribution audit remains required before publishing.",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), "files": len(files), "bytes": total_bytes}))


if __name__ == "__main__":
    main()
