#!/usr/bin/env python3
"""Run Bazel (xclang) builds with shared caches and stage binaries into build/<profile>."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

PROFILES = ("release", "debug", "releasedbg", "sanitize")

HOST_TARGETS: dict[str, list[str]] = {
    "engrave_mcp": ["//:engrave-mcp"],
    "engrave-mcp": ["//:engrave-mcp"],
    "engrave_mcp_test": ["//:engrave-mcp-test"],
    "engrave-mcp-test": ["//:engrave-mcp-test"],
    "host_api_test": ["//:host_api_test"],
    "live_export_test": ["//:live_export_test"],
    "staging_test": ["//:staging_test"],
    "publication_test": ["//:publication_test"],
    "registry_dump": ["//:registry_dump"],
    "registry_test": ["//:registry_test"],
    "registry_check": ["//:registry_dump", "//:registry_test"],
    "host_binaries": ["//:host_binaries"],
    "all_binaries": ["//:all_binaries"],
}


def resolve_targets(names: list[str]) -> list[str]:
    labels: list[str] = []
    for name in names:
        if name.startswith(("//", "@", ":")):
            expanded = [name]
        elif name in HOST_TARGETS:
            expanded = HOST_TARGETS[name]
        else:
            expanded = [f"@engrave//:{name}"]
        for label in expanded:
            if label not in labels:
                labels.append(label)
    return labels


def engine_git_revision(engine_dir: Path) -> str:
    try:
        sha = subprocess.check_output(
            ["git", "-C", str(engine_dir), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        return sha[:7] if len(sha) >= 7 else ""
    except (OSError, subprocess.CalledProcessError):
        return ""


def find_bazel(pixi_prefix: Path) -> str:
    found_bazel = shutil.which("bazel")
    if found_bazel:
        return found_bazel
    candidates = [
        pixi_prefix / "Library/bin/bazelisk.exe",
        pixi_prefix / "bin/bazelisk",
        pixi_prefix / "Library/bin/bazel.exe",
        pixi_prefix / "bin/bazel",
    ]
    for cand in candidates:
        if cand.is_file():
            return str(cand)
    found_bazelisk = shutil.which("bazelisk")
    if found_bazelisk:
        return found_bazelisk
    raise SystemExit("bazelisk/bazel not found; run `pixi install`")


def _uri_to_path(uri: str) -> Path | None:
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme != "file":
        return None
    raw = urllib.request.url2pathname(parsed.path)
    return Path(raw)


def stage_bep_outputs(bep_path: Path, out_dir: Path) -> int:
    named_sets: dict[str, dict] = {}
    root_ids: dict[str, list[str]] = {"default": [], "runtime_dynamic_libraries": []}
    for line in bep_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        if "namedSetOfFiles" in event:
            set_id = event.get("id", {}).get("namedSet", {}).get("id")
            if set_id:
                named_sets[set_id] = event["namedSetOfFiles"]
        elif "completed" in event:
            for group in event["completed"].get("outputGroup", []):
                if (name := group.get("name")) in root_ids:
                    for fs in group.get("fileSets", []):
                        if "id" in fs:
                            root_ids[name].append(fs["id"])

    visited: set[str] = set()
    # Default outputs take precedence over runtime files with the same basename.
    stack = root_ids["runtime_dynamic_libraries"] + root_ids["default"]
    files: list[Path] = []
    while stack:
        sid = stack.pop()
        if sid in visited or sid not in named_sets:
            continue
        visited.add(sid)
        node = named_sets[sid]
        for f in node.get("files", []):
            uri = f.get("uri")
            if uri:
                p = _uri_to_path(uri)
                if p is not None:
                    files.append(p)
        for child in node.get("fileSets", []):
            if "id" in child:
                stack.append(child["id"])

    out_dir.mkdir(parents=True, exist_ok=True)
    staged_names: set[str] = set()
    for src in files:
        if not src.is_file():
            continue
        if os.name == "nt" and src.suffix.lower() not in {".exe", ".dll"}:
            continue
        if os.name != "nt":
            shared_library = src.suffix == ".so" or ".so." in src.name
            if not shared_library and (src.suffix in {".a", ".o", ".h", ".pch"} or not os.access(src, os.X_OK)):
                continue
        name = src.name.casefold() if os.name == "nt" else src.name
        if name in staged_names:
            continue
        staged_names.add(name)
        dst = out_dir / src.name
        if dst.exists():
            try:
                if dst.stat().st_mtime_ns == src.stat().st_mtime_ns and dst.stat().st_size == src.stat().st_size:
                    continue
            except OSError:
                pass
            os.chmod(dst, 0o755)
            dst.unlink()
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
    return 0


def _resolve_bazel_path(rel: str, execroot: Path, output_base: Path) -> Path:
    if rel.startswith("external/"):
        ext_cand = output_base / rel
        if ext_cand.exists():
            return ext_cand.resolve()
    cand = execroot / rel
    if cand.exists():
        return cand.resolve()
    return cand


def _expand_clang_cfg(cfg_path: Path, execroot: Path, output_base: Path) -> list[str]:
    out: list[str] = []
    for raw_line in cfg_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        i = 0
        while i < len(tokens):
            tok = tokens[i]
            if tok in {"-stdlib++-isystem", "-isystem", "-idirafter", "-I"} and i + 1 < len(tokens):
                rel = tokens[i + 1]
                out.extend([tok, str(_resolve_bazel_path(rel, execroot, output_base)).replace("\\", "/")])
                i += 2
                continue
            for prefix in ("--sysroot=", "-resource-dir="):
                if tok.startswith(prefix):
                    rel = tok[len(prefix) :]
                    tok = prefix + str(_resolve_bazel_path(rel, execroot, output_base)).replace("\\", "/")
                    break
            out.append(tok)
            i += 1
    return out


def generate_compdb(
    bazel: str,
    root: Path,
    env: dict[str, str],
    compdb_flags: list[str],
    common_flags: list[str],
    labels: list[str],
    out_dir: Path,
) -> int:
    rc = subprocess.call([bazel, "build", "--nobuild", *compdb_flags, *labels], cwd=root, env=env)
    if rc != 0:
        return rc

    info_res = subprocess.run(
        [bazel, "info", *common_flags, "execution_root"],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
    )
    if info_res.returncode != 0:
        sys.stderr.write(info_res.stderr)
        return info_res.returncode
    execroot = Path(info_res.stdout.strip())
    output_base = execroot.parent.parent if execroot.parent.name == "execroot" else execroot
    root_resolved = root.resolve()
    root_posix = str(root_resolved).replace("\\", "/")

    query = f'mnemonic("CppCompile", deps({" + ".join(labels)}))'
    aq = subprocess.run(
        [bazel, "aquery", "--output=jsonproto", "--include_artifacts=false", *compdb_flags, query],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
    )
    if aq.returncode != 0:
        sys.stderr.write(aq.stderr)
        return aq.returncode

    cfg_cache: dict[str, list[str]] = {}
    seen_units: set[tuple[Path, tuple[str, ...]]] = set()
    data = json.loads(aq.stdout)
    entries: list[dict[str, object]] = []
    for action in data.get("actions", []):
        raw_args = list(action.get("arguments", []))
        if not raw_args:
            continue
        src_arg: str | None = None
        out_arg: str | None = None
        for idx, tok in enumerate(raw_args):
            if tok == "-c" and idx + 1 < len(raw_args):
                src_arg = raw_args[idx + 1]
            elif tok == "-o" and idx + 1 < len(raw_args):
                out_arg = raw_args[idx + 1]
        if not src_arg or src_arg.endswith((".h", ".hpp", ".S", ".s")):
            continue
        if out_arg and "-exec" in out_arg:
            continue
        real_src = _resolve_bazel_path(src_arg, execroot, output_base)
        if not real_src.is_file() or not real_src.is_relative_to(root_resolved):
            continue

        cooked: list[str] = []
        compiler_path = _resolve_bazel_path(raw_args[0], execroot, output_base)
        cooked.append(str(compiler_path if compiler_path.exists() else raw_args[0]).replace("\\", "/"))

        i = 1
        while i < len(raw_args):
            tok = raw_args[i]
            if tok.startswith("-frandom-seed="):
                i += 1
                continue
            if tok == "-include-pch" and i + 1 < len(raw_args):
                i += 2
                if raw_args[i : i + 4] == ["-Xclang", "-isysroot", "-Xclang", "."]:
                    i += 4
                continue
            if tok == "-Xclang" and i + 1 < len(raw_args) and raw_args[i + 1] == "-fno-validate-pch":
                i += 2
                continue
            if tok.startswith("--config="):
                rel_cfg = tok[len("--config=") :]
                if rel_cfg not in cfg_cache:
                    cfg_file = _resolve_bazel_path(rel_cfg, execroot, output_base)
                    cfg_cache[rel_cfg] = (
                        _expand_clang_cfg(cfg_file, execroot, output_base) if cfg_file.is_file() else [tok]
                    )
                cooked.extend(cfg_cache[rel_cfg])
                i += 1
                continue
            if tok in {"-I", "-iquote", "-isystem"} and i + 1 < len(raw_args):
                inc = raw_args[i + 1]
                resolved_inc = _resolve_bazel_path(inc, execroot, output_base)
                if (
                    resolved_inc.exists()
                    and resolved_inc.is_relative_to(root_resolved)
                    and "thirdparty" not in resolved_inc.parts
                ):
                    out_flag = "-I" if tok == "-isystem" else tok
                    cooked.extend([out_flag, str(resolved_inc).replace("\\", "/")])
                else:
                    cooked.extend([tok, str(resolved_inc).replace("\\", "/")])
                i += 2
                continue
            if tok.startswith("-I") and len(tok) > 2:
                inc = tok[2:]
                resolved_inc = _resolve_bazel_path(inc, execroot, output_base)
                cooked.append("-I" + str(resolved_inc).replace("\\", "/"))
                i += 1
                continue
            if tok == "-c" and i + 1 < len(raw_args):
                cooked.extend(["-c", str(real_src).replace("\\", "/")])
                i += 2
                continue
            cooked.append(tok)
            i += 1

        norm_args: list[str] = []
        skip_next = False
        for tok in cooked:
            if skip_next:
                skip_next = False
                continue
            if tok in {"-o", "-MF"}:
                skip_next = True
                continue
            if tok in {"-fPIC", "-fPIE"}:
                continue
            norm_args.append(tok)
        unit_key = (real_src, tuple(norm_args))
        if unit_key in seen_units:
            continue
        seen_units.add(unit_key)

        entry: dict[str, object] = {
            "directory": root_posix,
            "file": str(real_src).replace("\\", "/"),
            "arguments": cooked,
        }
        if out_arg:
            entry["output"] = str(execroot / out_arg).replace("\\", "/")
        entries.append(entry)

    out_dir.mkdir(parents=True, exist_ok=True)
    compdb_path = out_dir / "compile_commands.json"
    compdb_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
    print(f"Wrote {len(entries)} entries to {compdb_path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="release")
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("-j", "--jobs", type=int)
    parser.add_argument("--no-pch", action="store_true", help="Disable precompiled headers")
    parser.add_argument("--playback-harness", action="store_true", help="Enable ENGRAVE_PLAYBACK_HARNESS")
    parser.add_argument("action", choices=("build", "compdb", "fetch", "clean"))
    parser.add_argument("targets", nargs="*")
    args = parser.parse_intermixed_args(argv)

    root = Path(__file__).resolve().parents[1]
    pixi_prefix = root / ".pixi/envs/default"
    bazel = find_bazel(pixi_prefix)

    repo_cache = root / "build/.bazel_repo_cache"
    disk_cache = root / "build/.bazel_disk_cache"
    repo_cache.mkdir(parents=True, exist_ok=True)
    disk_cache.mkdir(parents=True, exist_ok=True)

    out_dir = (root / (args.build_directory or Path("build") / args.profile)).resolve()
    rev = engine_git_revision(root / "vendor/engrave")

    env = dict(os.environ)
    extra_paths = [pixi_prefix / "Library/bin", pixi_prefix / "bin"]
    env["PATH"] = os.pathsep.join(str(p) for p in extra_paths if p.is_dir()) + os.pathsep + env.get("PATH", "")

    common_flags = [
        f"--config={args.profile}",
        f"--repository_cache={repo_cache}",
        "--repo_contents_cache=",
        f"--disk_cache={disk_cache}",
        f"--repo_env=ENGRAVE_PIXI_PREFIX={pixi_prefix}",
        f"--repo_env=ENGRAVE_GIT_REVISION={rev}",
    ]
    if args.no_pch:
        common_flags.append("--@engrave//bazel:pch=false")
    if args.playback_harness:
        common_flags.append("--@engrave//bazel:playback_harness=true")

    build_flags = [
        *common_flags,
        f"--symlink_prefix={root / 'build' / f'.bazel-symlinks-{args.profile}'}/",
    ]
    if args.jobs:
        build_flags.append(f"--jobs={args.jobs}")

    if args.action == "clean":
        return subprocess.call([bazel, "clean"], cwd=root, env=env)

    requested_targets = args.targets or ["all_binaries"]
    labels = resolve_targets(requested_targets)

    if args.action == "fetch":
        return subprocess.call([bazel, "fetch", *common_flags, *labels], cwd=root, env=env)

    if args.action == "compdb":
        return generate_compdb(bazel, root, env, build_flags, common_flags, labels, out_dir)

    py = pixi_prefix / ("python.exe" if os.name == "nt" else "bin/python")
    py_exe = str(py) if py.is_file() else sys.executable
    rc = subprocess.call(
        [py_exe, str(root / "scripts/check_native_includes.py"), "--engine-source", str(root / "vendor/engrave")],
        cwd=root,
        env=env,
    )
    if rc != 0:
        return rc

    with tempfile.NamedTemporaryFile(dir=root / "build", prefix=".bep-", suffix=".json", delete=False) as tf:
        bep_path = Path(tf.name)
    try:
        rc = subprocess.call(
            [
                bazel,
                "build",
                *build_flags,
                "--output_groups=+runtime_dynamic_libraries",
                f"--build_event_json_file={bep_path}",
                *labels,
            ],
            cwd=root,
            env=env,
        )
        if rc != 0:
            return rc
        rc = stage_bep_outputs(bep_path, out_dir)
        if rc != 0:
            return rc
    finally:
        bep_path.unlink(missing_ok=True)

    if "registry_check" in requested_targets:
        ext = ".exe" if os.name == "nt" else ""
        check_env = dict(env)
        if os.name != "nt" and (pixi_prefix / "lib").is_dir():
            check_env["LD_LIBRARY_PATH"] = str(pixi_prefix / "lib") + os.pathsep + check_env.get("LD_LIBRARY_PATH", "")
        return subprocess.call(
            [
                py_exe,
                str(root / "tests/integration/test_tool_schema.py"),
                "--dump",
                str(out_dir / f"registry_dump{ext}"),
                "--registry-test",
                str(out_dir / f"registry_test{ext}"),
            ],
            cwd=root,
            env=check_env,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
