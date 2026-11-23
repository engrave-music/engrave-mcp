"""Validate versions and assemble release archives and checksum manifests."""

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKSUM_LINE = re.compile(r"([0-9a-fA-F]{64})  (.+)")


def clean_runtime_env() -> dict[str, str]:
    if os.name == "nt":
        system_root = os.environ["SystemRoot"]
        env = {key: os.environ[key] for key in ("TEMP", "TMP") if key in os.environ}
        return env | {"SystemRoot": system_root, "PATH": str(Path(system_root) / "System32")}
    return {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}


def check_tag(tag: str | None, binary: Path | None, clean_env: bool = False) -> str:
    # The artifact-only publish job can run without the editable testing package.
    from engrave_mcp_testing.version import read_host_version

    version = read_host_version(ROOT)
    if tag is not None and tag != f"v{version}":
        raise ValueError(f"expected tag v{version}, got {tag!r}")
    if binary is not None:
        binary = binary.resolve(strict=True)
        result = subprocess.run(
            [binary, "--version"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            cwd=binary.parent if clean_env else None,
            env=clean_runtime_env() if clean_env else None,
        )
        if result.returncode:
            raise ValueError(f"{binary} --version exited with code {result.returncode}: {result.stderr.strip()}")
        expected = f"engrave-mcp {version}"
        if result.stdout.strip() != expected:
            raise ValueError(f"expected {expected!r} from {binary}, got {result.stdout.strip()!r}")
    return version


WINDOWS_RESERVED_NAMES = frozenset(
    {
        "CON",
        "PRN",
        "AUX",
        "NUL",
        "CONIN$",
        "CONOUT$",
        *(f"{prefix}{digit}" for prefix in ("COM", "LPT") for digit in "123456789¹²³"),
    }
)


def _is_windows_reserved(name: str) -> bool:
    stem = name.split(".", 1)[0].rstrip(" ").upper()
    return stem in WINDOWS_RESERVED_NAMES


def safe_name(name: str) -> str:
    # Reject names that change meaning or cannot be extracted on Windows.
    if (
        not name
        or name in {".", ".."}
        or name.endswith((".", " "))
        or re.search(r'[\\/:*?"<>|\x00-\x1f]', name)
        or _is_windows_reserved(name)
    ):
        raise ValueError(f"unsafe release filename: {name!r}")
    return name


def archive_members(source: Path, root_name: str) -> list[tuple[str, Path, int]]:
    members = []
    pending = [source]
    while pending:
        path = pending.pop()
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f"links and reparse points are unsupported: {path}")
        directory = stat.S_ISDIR(info.st_mode)
        if path == source and not directory:
            raise ValueError(f"archive source is not a directory: {source}")
        if directory:
            pending.extend(path.iterdir())
        elif not stat.S_ISREG(info.st_mode):
            raise ValueError(f"unsupported archive member: {path}")
        name = "/".join([root_name, *(safe_name(part) for part in path.relative_to(source).parts)])
        permissions = 0o755 if directory or info.st_mode & 0o111 else 0o644
        members.append((name, path, stat.S_IFMT(info.st_mode) | permissions))
    if len(members) == 1:
        raise ValueError(f"archive source is empty: {source}")
    return sorted(members)


def pack(source: Path, output: Path, archive_format: str, root_name: str | None = None) -> None:
    if archive_format not in {"zip", "tar.gz"}:
        raise ValueError(f"unsupported archive format: {archive_format!r}")
    source = source.absolute()
    root_name = safe_name(source.name if root_name is None else root_name)
    if output.resolve().is_relative_to(source.resolve()):
        raise ValueError(f"archive output must be outside the source directory: {output}")
    members = archive_members(source, root_name)

    stream = output.open("xb")
    try:
        with stream:
            if archive_format == "zip":
                with zipfile.ZipFile(stream, "w") as archive:
                    for name, path, mode in members:
                        directory = stat.S_ISDIR(mode)
                        info = zipfile.ZipInfo(name + ("/" if directory else ""))
                        info.create_system = 3  # Unix permission bits in external_attr.
                        info.external_attr = mode << 16
                        if directory:
                            info.external_attr |= 0x10  # DOS directory attribute.
                            archive.writestr(info, b"")
                        else:
                            info.compress_type = zipfile.ZIP_DEFLATED
                            with path.open("rb") as data:
                                info.file_size = os.fstat(data.fileno()).st_size
                                with archive.open(info, "w") as member:
                                    shutil.copyfileobj(data, member)
            elif archive_format == "tar.gz":
                with gzip.GzipFile(filename="", fileobj=stream, mode="wb", mtime=0) as gzip_stream:
                    with tarfile.open(fileobj=gzip_stream, mode="w") as archive:
                        for name, path, mode in members:
                            info = tarfile.TarInfo(name)
                            info.mode = stat.S_IMODE(mode)
                            if stat.S_ISDIR(mode):
                                info.type = tarfile.DIRTYPE
                                archive.addfile(info)
                            else:
                                with path.open("rb") as data:
                                    info.size = os.fstat(data.fileno()).st_size
                                    archive.addfile(info, data)
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def sha256(path: Path) -> str:
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def checksums(output: Path, files: list[Path]) -> None:
    entries = {}
    for path in files:
        name = safe_name(path.name)
        if name in entries:
            raise ValueError(f"duplicate checksum filename: {name!r}")
        entries[name] = sha256(path)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        stream.writelines(f"{entries[name]}  {name}\n" for name in sorted(entries))


def verify_checksums(manifest: Path, directory: Path) -> None:
    seen = set()
    with manifest.open(encoding="utf-8", newline="") as stream:
        for number, line in enumerate(stream, start=1):
            match = CHECKSUM_LINE.fullmatch(line.removesuffix("\n"))
            if not match:
                raise ValueError(f"{manifest}:{number}: malformed checksum line")
            digest, name = match.groups()
            path = directory / safe_name(name)
            if name in seen:
                raise ValueError(f"duplicate checksum filename: {name!r}")
            if not path.is_file() or path.is_symlink():
                raise ValueError(f"expected a regular file in {directory}: {name!r}")
            if sha256(path) != digest.lower():
                raise ValueError(f"SHA-256 mismatch: {path}")
            seen.add(name)
    if not seen:
        raise ValueError(f"empty checksum manifest: {manifest}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    tag = commands.add_parser("check-tag")
    tag.add_argument("--tag")
    tag.add_argument("--binary", type=Path)
    tag.add_argument("--clean-env", action="store_true", help="Run the binary with a system-only environment")
    tag.add_argument("--json", action="store_true")
    archive = commands.add_parser("pack")
    archive.add_argument("--source", required=True, type=Path)
    archive.add_argument("--output", required=True, type=Path)
    archive.add_argument("--format", required=True, choices=("zip", "tar.gz"))
    archive.add_argument("--root-name")
    sums = commands.add_parser("checksums")
    sums.add_argument("--output", required=True, type=Path)
    sums.add_argument("files", nargs="+", type=Path)
    verify = commands.add_parser("verify-checksums")
    verify.add_argument("--checksums", required=True, type=Path)
    verify.add_argument("--dir", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        match args.command:
            case "check-tag":
                version = check_tag(args.tag, args.binary, args.clean_env)
                print(json.dumps({"version": version, "prerelease": "-" in version}) if args.json else version)
            case "pack":
                pack(args.source, args.output, args.format, args.root_name)
            case "checksums":
                checksums(args.output, args.files)
            case "verify-checksums":
                verify_checksums(args.checksums, args.dir)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
