#!/usr/bin/env python3
"""Replay the pinned 4.7.6 oracle or verify externally captured write470 results.

Baseline mode tests this harness with reviewed 4.70 projections and synthetic
warnings. It is not a writer test. Writer-results mode requires one actual
result envelope, warning file, and (on success) MSCZ per case; B2 must connect
its real export paths to this interface.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_SHA = "b915ae9f4ec2fdbfd5fc0edd4be19c3d9026f4e154174967b47556915ac719fa"
BINARY_SHA = "f6d9ee1e826e27074cb2a9481987ddbd9318544e9c3586dc16ab534c3fd7fe8a"
CREATED_SHA = "a8d841d0af0b56744dff7ead93373e747a8196fae4bcfabf79fcca822d5b7d21"
CREATED_SEMANTIC_SHA = "e1a79ef26cc7eef398aee8122bb75477e53e85a4aff059e9a1e8c6d8e9070040"


class GateError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    require(path.is_file(), f"missing file: {path}")
    return sha(path.read_bytes())


def read_json(path: Path):
    def reject(value):
        raise ValueError(f"non-finite JSON value: {value}")
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject)


def save_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def inside(root: Path, name: str) -> Path:
    result = (root / name).resolve()
    require(result.is_relative_to(root.resolve()), f"path escapes fixture directory: {name}")
    return result


def check_file(root: Path, case: dict, field: str) -> Path:
    path = inside(root, case[field])
    actual = file_sha(path)
    require(actual == case[field + "_sha256"], f"{case['id']}: {field} hash mismatch: {path}")
    return path


def preflight_oracle(archive: Path, binary: Path, assets: Path) -> Path:
    require(sys.platform.startswith("linux"), "the preserved oracle is Linux ELF; execute this lane on physical linux-host")
    require(file_sha(archive) == ARCHIVE_SHA, f"oracle archive SHA-256 mismatch: {archive}")
    binary, assets = binary.resolve(), assets.resolve()
    root = binary.parent.parent
    require(binary == root / "bin/semantic_dump" and assets == root / "assets", "oracle binary and assets must be from the same extracted archive")
    require(assets.is_dir() and (root / "lib").is_dir(), "oracle assets or lib directory missing")
    with tarfile.open(archive, "r:gz") as tar:
        member = tar.extractfile("mcp-4.7.6-4/SHA256SUMS")
        require(member is not None, "archive lacks SHA256SUMS")
        archived_sums = member.read()
    sums_path = root / "SHA256SUMS"
    require(sums_path.is_file() and sums_path.read_bytes() == archived_sums, "extracted SHA256SUMS differs from pinned archive")
    expected = {}
    for line in archived_sums.decode("utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  \./(.+)", line)
        require(match is not None, f"invalid oracle SHA256SUMS line: {line}")
        digest, name = match.groups()
        require(name not in expected, f"duplicate oracle member: {name}")
        expected[name] = digest
    require(len(expected) == 87, f"oracle inventory has {len(expected)} entries, expected 87")
    actual_files = set()
    for path in root.rglob("*"):
        require(not path.is_symlink(), f"oracle contains a symlink: {path}")
        if path.is_file():
            actual_files.add(path.relative_to(root).as_posix())
    require(actual_files == set(expected) | {"SHA256SUMS"}, "oracle extracted file inventory differs from SHA256SUMS")
    for name, digest in expected.items():
        path = inside(root, name)
        require(file_sha(path) == digest, f"oracle checksum mismatch: {name}")
    require(expected.get("bin/semantic_dump") == BINARY_SHA and file_sha(binary) == BINARY_SHA,
            "oracle executable is not the pinned 4.7.6 binary")
    require(os.access(binary, os.X_OK), f"oracle is not executable: {binary}")
    return root


def validate_manifest(root: Path, manifest_path: Path):
    manifest = read_json(manifest_path)
    require(manifest.get("schema") == 1 and manifest.get("oracle_archive_sha256") == ARCHIVE_SHA
            and manifest.get("oracle_binary_sha256") == BINARY_SHA, "invalid oracle manifest identity")
    cases = manifest.get("cases")
    require(isinstance(cases, list) and cases, "manifest selects zero cases")
    identities = manifest.get("warnings")
    require(isinstance(identities, dict) and identities, "warning identities are missing")
    seen = set()
    for case in cases:
        name = case.get("id", "")
        require(re.fullmatch(r"[a-z0-9_]+", name) is not None and name not in seen, f"invalid or duplicate case ID: {name}")
        seen.add(name)
        require(case.get("category") in ("preserved", "degraded", "rejected"), f"{name}: invalid category")
        for field in ("source", "oracle_input", "expected_semantics", "warnings"):
            check_file(root, case, field)
        require(isinstance(case.get("target_version"), str), f"{name}: target version is missing")
        if "request" in case:
            request = read_json(check_file(root, case, "request"))
            require(request.get("version") == case["target_version"], f"{name}: request version mismatch")
        if case["category"] == "degraded":
            check_file(root, case, "source_oracle")
        warnings = read_json(inside(root, case["warnings"]))
        require(isinstance(warnings, list), f"{name}: warnings must be a list")
        require(bool(warnings) == (case["category"] != "preserved"), f"{name}: warnings do not match category")
        keys = set()
        for warning in warnings:
            require(isinstance(warning, dict) and set(warning) == {"code", "message", "object"}, f"{name}: invalid warning")
            code, obj = warning["code"], warning["object"]
            require(code in identities and warning["message"] == identities[code], f"{name}: warning identity or message is not frozen")
            require(isinstance(obj, dict) and set(obj) == {"kind", "id"}
                    and all(isinstance(obj[key], str) and obj[key] for key in ("kind", "id")), f"{name}: invalid warning object")
            key = (code, obj["kind"], obj["id"])
            require(key not in keys, f"{name}: duplicate warning object")
            keys.add(key)
    return cases


def run_dump(binary: Path, assets: Path, source: Path, log_dir: Path, label: str, env: dict[str, str]):
    proc = subprocess.run([str(binary), str(assets), str(source)], env=env, capture_output=True, timeout=60)
    (log_dir / f"{label}.stdout").write_bytes(proc.stdout)
    (log_dir / f"{label}.stderr").write_bytes(proc.stderr)
    return proc


def compare_semantics(expected, actual) -> None:
    canonical = lambda value: json.dumps(value, sort_keys=True, ensure_ascii=False,
                                         separators=(",", ":"), allow_nan=False)
    require(canonical(expected) == canonical(actual), "normalized semantics differ")


def compare_warnings(expected, actual) -> None:
    require(isinstance(actual, list), "warnings must be an array")
    canon = lambda obj: json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    require(sorted(map(canon, expected)) == sorted(map(canon, actual)), "warning identities, objects or messages differ")


def xml_roots(path: Path):
    if path.suffix.lower() == ".mscz":
        with zipfile.ZipFile(path) as archive:
            members = [name for name in archive.namelist() if name.lower().endswith(".mscx")]
            require(members, f"MSCZ has no score XML: {path}")
            return [ET.fromstring(archive.read(name)) for name in members]
    return [ET.parse(path).getroot()]


def compare_xml(path: Path, checks: dict) -> None:
    roots = xml_roots(path)
    require(all(root.tag == "museScore" and root.get("version") == "4.70" for root in roots), "output is not declared 4.70")
    forbidden = {"SharedPart", "sharedPart", "StaveSharingLabel", "PageLocks", "pageLock", "InstrumentLabel",
                 "customNameLongGroup", "customNameShortGroup", "useCustomGroupName"}
    for root in roots:
        for element in root.iter():
            require(element.tag not in forbidden, f"5.00-only element in output: {element.tag}")
            if element.tag == "BracketItem":
                require(element.findtext("type") != "Group", "5.00 group bracket in output")
    if checks.get("eids"):
        eids = {element.text for root in roots for element in root.iter("eid")}
        require(set(checks["eids"]).issubset(eids), "expected EIDs missing from output XML")
    if checks.get("ordinary_bracket"):
        b = checks["ordinary_bracket"]
        found = False
        for root in roots:
            for part in root.iter("Part"):
                for staff in part.findall("Staff"):
                    if staff.findtext("eid") != b["staff_eid"]:
                        continue
                    for bracket in staff.findall("bracket"):
                        if (bracket.get("type") == "1" and bracket.get("span") == str(b["span"])
                                and bracket.get("col", "0") == str(b["column"])):
                            found = True
        require(found, "expected ordinary 4.70 brace missing from output XML")


def expect_comparator_rejection(label: str, action) -> str:
    try:
        action()
    except GateError:
        return label
    raise GateError(f"comparator accepted mutation: {label}")


def mutation_tests(root: Path, case_dir: Path, binary: Path, assets: Path, oracle_env: dict[str, str]):
    expected = read_json(root / "expected/notes.json")
    # A real 4.70 MSCZ reopen is a reader control in every mode. This wrapper
    # is scratch input, not evidence that the current writer produced it.
    wrapped = case_dir / "control.mscz"
    with zipfile.ZipFile(wrapped, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("score.mscx", (root / "notes.mscx").read_bytes())
        archive.writestr("META-INF/container.xml",
                         '<?xml version="1.0" encoding="UTF-8"?>\n'
                         '<container><rootfiles><rootfile full-path="score.mscx"/></rootfiles></container>')
    control = run_dump(binary, assets, wrapped, case_dir, "control-oracle", oracle_env)
    require(control.returncode == 0 and not control.stderr, "oracle MSCZ control failed or reported diagnostics")
    compare_semantics(expected, json.loads(control.stdout))
    altered = copy.deepcopy(expected)
    altered["events"][0][0][0]["notes"].clear()
    names = [expect_comparator_rejection("lost_note", lambda: compare_semantics(expected, altered))]
    altered = copy.deepcopy(expected)
    require(bool(altered["parts"][0]["longName"]), "name mutation is vacuous")
    altered["parts"][0]["longName"] = ""
    names.append(expect_comparator_rejection("lost_name", lambda: compare_semantics(expected, altered)))
    warnings = read_json(root / "expected/page_locks.warnings.json")
    require(len(warnings) >= 2, "warning mutations need multiple affected objects")
    names.append(expect_comparator_rejection("missing_warning", lambda: compare_warnings(warnings, warnings[1:])))
    altered_warnings = copy.deepcopy(warnings)
    altered_warnings[0]["object"]["id"] = "wrong-object"
    names.append(expect_comparator_rejection("wrong_object_warning", lambda: compare_warnings(warnings, altered_warnings)))

    corpus = ROOT / "tests/compat-corpus/engrave-19"
    source, reference = corpus / "created.json.mscz", corpus / "created.json.semantic.json"
    require(file_sha(source) == CREATED_SHA and file_sha(reference) == CREATED_SEMANTIC_SHA, "spoof reference is missing or differs from engrave-19")
    with zipfile.ZipFile(source) as archive:
        xml = archive.read("score.mscx")
    require(xml.count(b'version="5.00"') == 1, "spoof source has an unexpected header")
    spoof = case_dir / "spoof.mscx"
    spoof.write_bytes(xml.replace(b'version="5.00"', b'version="4.70"', 1))
    proc = run_dump(binary, assets, spoof, case_dir, "spoof-oracle", oracle_env)
    require(proc.returncode == 0 and not proc.stderr, "oracle did not silently accept the frozen relabel control")
    spoof_semantics = json.loads(proc.stdout)
    reference_semantics = read_json(reference)
    names.append(expect_comparator_rejection("header_spoof_acceptance", lambda: compare_semantics(reference_semantics, spoof_semantics)))
    return {"rejected_mutations": names, "control_mscz_sha256": file_sha(wrapped),
            "control_exit": control.returncode, "control_stdout_sha256": sha(control.stdout),
            "control_stderr_sha256": sha(control.stderr),
            "spoof_source_sha256": sha(xml), "spoof_sha256": file_sha(spoof),
            "spoof_stdout_sha256": sha(proc.stdout), "spoof_stderr_sha256": sha(proc.stderr), "spoof_exit": proc.returncode,
            "reference_semantics_sha256": file_sha(reference)}


def run_case(case: dict, fixture_root: Path, output: Path, binary: Path, assets: Path,
             oracle_env: dict[str, str], current_binary: Path | None, current_assets: Path | None,
             baseline: bool, results: Path | None):
    name = case["id"]
    dest = output / name
    dest.mkdir()
    source = check_file(fixture_root, case, "source")
    oracle_input = check_file(fixture_root, case, "oracle_input")
    expected_path = check_file(fixture_root, case, "expected_semantics")
    warnings_path = check_file(fixture_root, case, "warnings")
    expected = read_json(expected_path)
    expected_warnings = read_json(warnings_path)
    record = {"case": name, "category": case["category"], "source_sha256": file_sha(source),
              "oracle_input_sha256": file_sha(oracle_input), "expected_semantics_sha256": file_sha(expected_path),
              "expected_warning_sha256": file_sha(warnings_path), "target_version": case["target_version"],
              "request_sha256": file_sha(inside(fixture_root, case["request"])) if "request" in case else None,
              "mode": "baseline" if baseline else "writer_results"}
    try:
        if case["category"] == "degraded":
            source_observation = read_json(check_file(fixture_root, case, "source_oracle"))
            proc = run_dump(binary, assets, source, dest, "source-oracle", oracle_env)
            record["source_oracle"] = {"exit": proc.returncode, "stdout_sha256": sha(proc.stdout), "stderr_sha256": sha(proc.stderr)}
            require(proc.returncode == source_observation["exit"] and proc.stdout.decode("utf-8") == source_observation["stdout"]
                    and proc.stderr.decode("utf-8") == source_observation["stderr"], f"{name}: source oracle rejection changed")
        if current_binary is not None:
            proc = run_dump(current_binary, current_assets, source, dest, "current", os.environ.copy())
            record["current"] = {"exit": proc.returncode, "stdout_sha256": sha(proc.stdout), "stderr_sha256": sha(proc.stderr)}
            require(proc.returncode == 0, f"{name}: current engine cannot read source")
            json.loads(proc.stdout)

        if baseline:
            actual_warnings = expected_warnings
            warning_bytes = warnings_path.read_bytes()
            score = None if case["category"] == "rejected" else oracle_input
            # The rejected tie's 4.7.6 observation is still captured as source evidence.
            read_path = oracle_input
        else:
            case_results = results / name
            result_file = case_results / "result.json"
            envelope = read_json(result_file)
            record["writer_result_sha256"] = file_sha(result_file)
            record["writer_outcome"] = envelope.get("outcome")
            (dest / "writer-result.json").write_bytes(result_file.read_bytes())
            require(envelope.get("source_sha256") == record["source_sha256"], f"{name}: result source hash mismatch")
            require(envelope.get("target_version") == case["target_version"], f"{name}: result target version mismatch")
            outcome = "rejected" if case["category"] == "rejected" else "written"
            require(envelope.get("outcome") == outcome, f"{name}: wrong writer outcome")
            warning_file = case_results / "warnings.json"
            warning_bytes = warning_file.read_bytes()
            actual_warnings = read_json(warning_file)
            score = case_results / "score.mscz"
            if outcome == "rejected":
                extras = {path.name for path in case_results.iterdir()} - {"result.json", "warnings.json"}
                require(not extras, f"{name}: rejected export published an artifact: {sorted(extras)}")
                score = None
            else:
                require(score.is_file(), f"{name}: writer output missing")
            read_path = score
        (dest / "warnings.json").write_bytes(warning_bytes)
        record["warning_sha256"] = sha(warning_bytes)
        compare_warnings(expected_warnings, actual_warnings)
        record["output_sha256"] = file_sha(score) if score else None
        if read_path is not None:
            if score is not None:
                compare_xml(score, case.get("xml_checks", {}))
            proc = run_dump(binary, assets, read_path, dest, "output-oracle", oracle_env)
            record["output_oracle"] = {"exit": proc.returncode, "stdout_sha256": sha(proc.stdout), "stderr_sha256": sha(proc.stderr)}
            require(proc.returncode == 0 and not proc.stderr, f"{name}: oracle failed or reported diagnostics")
            compare_semantics(expected, json.loads(proc.stdout))
        record["status"] = "passed"
    except Exception as error:
        record["status"] = "failed"
        record["error"] = str(error)
        raise
    finally:
        save_json(dest / "record.json", record)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--oracle-archive", type=Path, required=True)
    parser.add_argument("--oracle-executable", type=Path, required=True)
    parser.add_argument("--oracle-assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new, nonexistent output directory")
    parser.add_argument("--fixture-root", type=Path, default=ROOT / "tests/phase_b")
    parser.add_argument("--manifest", type=Path, help="override manifest for negative tests")
    parser.add_argument("--current-binary", type=Path)
    parser.add_argument("--current-assets", type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--baseline", action="store_true", help="oracle projections and synthetic warnings; not a writer gate")
    mode.add_argument("--writer-results", type=Path, help="actual export result directories from the writer")
    args = parser.parse_args()
    try:
        require((args.current_binary is None) == (args.current_assets is None), "specify both current binary and current assets")
        fixture_root = args.fixture_root.resolve()
        manifest_path = args.manifest.resolve() if args.manifest else fixture_root / "manifest.json"
        cases = validate_manifest(fixture_root, manifest_path)
        oracle_root = preflight_oracle(args.oracle_archive.resolve(), args.oracle_executable, args.oracle_assets)
        binary, assets = args.oracle_executable.resolve(), args.oracle_assets.resolve()
        oracle_env = {"LD_LIBRARY_PATH": str(oracle_root / "lib")}
        current_binary = args.current_binary.resolve() if args.current_binary else None
        current_assets = args.current_assets.resolve() if args.current_assets else None
        if current_binary:
            require(current_binary.is_file() and current_assets.is_dir(), "current binary or assets are missing")
        if args.writer_results:
            require(args.writer_results.is_dir(), "writer results directory is missing")
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        mutation_dir = output / "mutations"
        mutation_dir.mkdir()
        mutations = mutation_tests(fixture_root, mutation_dir, binary, assets, oracle_env)
        save_json(mutation_dir / "record.json", mutations)
        results = []
        for case in cases:
            results.append(run_case(case, fixture_root, output, binary, assets, oracle_env, current_binary,
                                    current_assets, args.baseline, args.writer_results.resolve() if args.writer_results else None))
            print(f"PASS {case['id']}", flush=True)
        summary = {"mode": "baseline_only" if args.baseline else "writer_results_verification", "cases": len(results),
                   "oracle_archive_sha256": ARCHIVE_SHA, "oracle_binary_sha256": BINARY_SHA,
                   "current_binary_sha256": file_sha(current_binary) if current_binary else None,
                   "mutations": mutations["rejected_mutations"]}
        save_json(output / "summary.json", summary)
        print(f"PASS {len(results)} cases; {summary['mode']} (not phase B acceptance)")
        return 0
    except (GateError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile, ET.ParseError) as error:
        print(f"phase B oracle lane FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
