"""Check the original MSCZ bytes and semantic dumps from every earlier engine pin.

Usage: test_compat_corpus.py --binary PATH/semantic_dump --resources ASSETS
                            [--corpus tests/compat-corpus] [--output DIR]
The lane also runs negative controls against a temporary copy of the corpus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHA = re.compile(r"[0-9a-f]{7,40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False,
                      separators=(",", ":"))


def reject_constant(value: str):
    raise ValueError(f"invalid JSON constant {value}")


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject_constant)


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=1, sort_keys=True, ensure_ascii=False,
                               allow_nan=False) + "\n", encoding="utf-8", newline="\n")


def safe_name(value, suffix: str) -> bool:
    return (isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9._-]+", value) is not None
            and value not in (".", "..") and value.endswith(suffix))


def semantic_dump(binary: Path, resources: Path, source: Path):
    result = subprocess.run([str(binary), str(resources), str(source)],
                            capture_output=True, timeout=120, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise ValueError(f"unreadable score (exit {result.returncode}): {detail[-800:]}")
    return json.loads(result.stdout.decode("utf-8"))


def check_corpus(root: Path, binary: Path, resources: Path, output: Path | None = None,
                 verbose: bool = True) -> tuple[list[str], int]:
    errors = []
    checked = 0
    if not root.is_dir():
        return [f"corpus directory missing: {root}"], checked
    pins = sorted(root.iterdir())
    if not pins:
        return ["corpus contains no pin directories"], checked
    for pin in pins:
        if not pin.is_dir() or pin.is_symlink():
            errors.append(f"unexpected corpus entry: {pin.name}")
            continue
        try:
            manifest = load_json(pin / "manifest.json")
            if not isinstance(manifest, dict) or manifest.get("schema") != 1:
                raise ValueError("unsupported manifest schema")
            entries = manifest.get("entries")
            if not isinstance(entries, list) or not entries:
                raise ValueError("manifest has no entries")
        except (OSError, ValueError, TypeError) as exc:
            errors.append(f"{pin.name}: invalid or missing manifest: {exc}")
            continue

        names = {"manifest.json"}
        valid = []
        for entry in entries:
            if not isinstance(entry, dict):
                errors.append(f"{pin.name}: invalid manifest entry")
                continue
            file = entry.get("file")
            semantic = entry.get("semantic")
            label = f"{pin.name}/{file}"
            if not safe_name(file, ".mscz") or not safe_name(semantic, ".json"):
                errors.append(f"{label}: invalid file or semantic name")
                continue
            if file in names or semantic in names or file == semantic:
                errors.append(f"{label}: duplicate manifest file")
                continue
            names.update((file, semantic))
            if (not isinstance(entry.get("sha256"), str)
                    or not SHA256.fullmatch(entry["sha256"])
                    or any(not isinstance(entry.get(key), str) or not SHA.fullmatch(entry[key])
                           for key in ("writer_sha", "upstream_sha", "framework_sha",
                                       "superproject_commit"))
                    or not isinstance(entry.get("features"), list)
                    or not entry["features"]
                    or any(not isinstance(feature, str) or not feature for feature in entry["features"])):
                errors.append(f"{label}: invalid provenance, hash or features")
                continue
            valid.append(entry)

        actual_names = {path.name for path in pin.iterdir()}
        for name in sorted(names - actual_names):
            errors.append(f"{pin.name}/{name}: missing file")
        for name in sorted(actual_names - names):
            errors.append(f"{pin.name}/{name}: extra file")
        for entry in valid:
            file = pin / entry["file"]
            expected_path = pin / entry["semantic"]
            label = f"{pin.name}/{file.name}"
            if not file.is_file() or file.is_symlink() or not expected_path.is_file() or expected_path.is_symlink():
                errors.append(f"{label}: missing or non-regular score/semantic file")
                continue
            try:
                if digest(file) != entry["sha256"]:
                    errors.append(f"{label}: sha256 mismatch")
                    continue
                expected = load_json(expected_path)
                if not isinstance(expected, dict) or expected.get("schema") != 1:
                    raise ValueError("invalid semantic dump schema")
                actual = semantic_dump(binary, resources, file)
                if output:
                    write_json(output / pin.name / entry["semantic"], actual)
                if canonical(actual) != canonical(expected):
                    errors.append(f"{label}: semantic dump mismatch")
                    continue
            except (OSError, ValueError, TypeError, subprocess.SubprocessError) as exc:
                errors.append(f"{label}: {exc}")
                continue
            checked += 1
            if verbose:
                print(f"ok {label}")
    return errors, checked


def negative_controls(root: Path, binary: Path, resources: Path) -> list[str]:
    """Each control must fail for its intended reason, using actual score reads."""
    first_pin = next(path for path in sorted(root.iterdir()) if path.is_dir())
    manifest = load_json(first_pin / "manifest.json")
    entries = manifest["entries"][:2]
    failures = []
    with tempfile.TemporaryDirectory(prefix="compat-controls-") as temporary:
        temporary = Path(temporary)
        base = temporary / "base" / first_pin.name
        base.mkdir(parents=True)
        write_json(base / "manifest.json", {**manifest, "entries": entries})
        for entry in entries:
            for name in (entry["file"], entry["semantic"]):
                shutil.copyfile(first_pin / name, base / name)
        if len(entries) == 1:
            extra = {**entries[0], "file": "control-copy.mscz",
                     "semantic": "control-copy.json"}
            shutil.copyfile(base / entries[0]["file"], base / extra["file"])
            shutil.copyfile(base / entries[0]["semantic"], base / extra["semantic"])
            entries.append(extra)
            write_json(base / "manifest.json", {**manifest, "entries": entries})
        errors, count = check_corpus(base.parent, binary, resources, verbose=False)
        if errors or count != len(entries):
            return [f"negative-control baseline failed: {errors}"]

        controls = (("tampered byte", "sha256 mismatch"),
                    ("missing entry", "extra file"),
                    ("missing file", "missing file"),
                    ("extra file", "extra file"),
                    ("unreadable score", "unreadable score"),
                    ("changed expected field", "semantic dump mismatch"))
        for index, (name, reason) in enumerate(controls):
            copy = temporary / f"control-{index}"
            shutil.copytree(base.parent, copy)
            pin = copy / first_pin.name
            path = pin / entries[0]["file"]
            changed = load_json(pin / "manifest.json")
            if name == "tampered byte":
                data = bytearray(path.read_bytes())
                data[0] ^= 1
                path.write_bytes(data)
            elif name == "missing entry":
                changed["entries"].pop(0)
                write_json(pin / "manifest.json", changed)
            elif name == "missing file":
                path.unlink()
            elif name == "extra file":
                (pin / "unexpected.mscz").write_bytes(b"unexpected")
            elif name == "unreadable score":
                path.write_bytes(b"not an MSCZ")
                changed["entries"][0]["sha256"] = digest(path)
                write_json(pin / "manifest.json", changed)
            else:
                expected_path = pin / entries[0]["semantic"]
                expected = load_json(expected_path)
                expected["metadata"]["workTitle"] += " (negative control)"
                write_json(expected_path, expected)
            errors, _ = check_corpus(copy, binary, resources, verbose=False)
            if not any(reason in error for error in errors):
                failures.append(f"negative control {name} did not fail for {reason}: {errors}")
            else:
                print(f"negative control passed: {name}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="current semantic_dump executable")
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, default=ROOT / "tests/compat-corpus")
    parser.add_argument("--output", type=Path, help="save actual semantic dumps for inspection")
    args = parser.parse_args()
    binary, resources, corpus = (path.resolve() for path in (args.binary, args.resources, args.corpus))
    errors, checked = check_corpus(corpus, binary, resources, args.output.resolve() if args.output else None)
    if not errors:
        errors.extend(negative_controls(corpus, binary, resources))
    for error in errors:
        print(f"FAIL {error}", file=sys.stderr)
    print(f"compat corpus: {checked} checked, {len(errors)} failure(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
