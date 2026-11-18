#!/usr/bin/env python3
"""Run Bazel (xclang) builds with shared caches and stage binaries into build/bazel-<profile>."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

PROFILES = ("release", "debug", "releasedbg", "asan")

HOST_TARGETS = {
    "engrave_mcp": "//:engrave-mcp",
    "engrave-mcp": "//:engrave-mcp",
    "engrave_mcp_test": "//:engrave-mcp-test",
    "engrave-mcp-test": "//:engrave-mcp-test",
    "host_api_test": "//:host_api_test",
    "live_export_test": "//:live_export_test",
    "staging_test": "//:staging_test",
    "publication_test": "//:publication_test",
    "registry_dump": "//:registry_dump",
    "registry_test": "//:registry_test",
    "host_binaries": "//:host_binaries",
    "all_binaries": "//:all_binaries",
}


def resolve_target(name: str) -> str:
    if name.startswith(("//", "@", ":")):
        return name
    if name in HOST_TARGETS:
        return HOST_TARGETS[name]
    return f"@engrave//:{name}"


def seed_repo_cache_from_xmake(repo_cache: Path) -> None:
    stamp = repo_cache / ".seeded_from_xmake"
    if stamp.is_file():
        return
    cas = repo_cache / "content_addressable" / "sha256"
    candidates = [
        Path.home() / "AppData/Local/.xmake/cache/packages",
        Path.home() / ".xmake/cache/packages",
    ]
    for base in candidates:
        if not base.is_dir():
            continue
        for archive in base.rglob("*"):
            if not archive.is_file() or archive.suffix not in {".gz", ".xz", ".zip"}:
                continue
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            slot = cas / digest / "file"
            if not slot.is_file():
                slot.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(archive, slot)
    stamp.write_text("1\n", encoding="utf-8")


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
    candidates = [
        pixi_prefix / "Library/bin/bazelisk.exe",
        pixi_prefix / "bin/bazelisk",
        pixi_prefix / "Library/bin/bazel.exe",
        pixi_prefix / "bin/bazel",
    ]
    for cand in candidates:
        if cand.is_file():
            return str(cand)
    found = shutil.which("bazelisk") or shutil.which("bazel")
    if found:
        return found
    raise SystemExit("bazelisk/bazel not found; run `pixi install`")


def _uri_to_path(uri: str) -> Path | None:
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme != "file":
        return None
    raw = urllib.request.url2pathname(parsed.path)
    return Path(raw)


def stage_bep_outputs(bep_path: Path, out_dir: Path) -> int:
    named_sets: dict[str, dict] = {}
    root_ids: list[str] = []
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
                if group.get("name") == "default":
                    for fs in group.get("fileSets", []):
                        if "id" in fs:
                            root_ids.append(fs["id"])

    visited: set[str] = set()
    stack = list(root_ids)
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
    for src in files:
        if not src.is_file():
            continue
        if os.name == "nt" and src.suffix.lower() not in {".exe", ".dll"}:
            continue
        if os.name != "nt" and (src.suffix in {".a", ".o", ".h", ".pch"} or not os.access(src, os.X_OK)):
            continue
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


def _expand_clang_cfg(cfg_path: Path, execroot: Path) -> list[str]:
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
                cand = execroot / rel
                out.extend([tok, str(cand.resolve() if cand.exists() else cand).replace("\\", "/")])
                i += 2
                continue
            for prefix in ("--sysroot=", "-resource-dir="):
                if tok.startswith(prefix):
                    rel = tok[len(prefix) :]
                    cand = execroot / rel
                    tok = prefix + str(cand.resolve() if cand.exists() else cand).replace("\\", "/")
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
        real_src = (execroot / src_arg).resolve()
        if not real_src.is_file() or not real_src.is_relative_to(root_resolved):
            continue

        cooked: list[str] = []
        compiler_path = execroot / raw_args[0]
        cooked.append(str(compiler_path.resolve() if compiler_path.exists() else raw_args[0]).replace("\\", "/"))

        i = 1
        while i < len(raw_args):
            tok = raw_args[i]
            if tok.startswith("-frandom-seed="):
                i += 1
                continue
            if tok.startswith("--config="):
                rel_cfg = tok[len("--config=") :]
                if rel_cfg not in cfg_cache:
                    cfg_file = execroot / rel_cfg
                    cfg_cache[rel_cfg] = _expand_clang_cfg(cfg_file, execroot) if cfg_file.is_file() else [tok]
                cooked.extend(cfg_cache[rel_cfg])
                i += 1
                continue
            if tok in {"-I", "-iquote", "-isystem"} and i + 1 < len(raw_args):
                inc = raw_args[i + 1]
                cand = execroot / inc
                if cand.exists():
                    resolved_inc = cand.resolve()
                    if resolved_inc.is_relative_to(root_resolved) and "thirdparty" not in resolved_inc.parts:
                        out_flag = "-I" if tok == "-isystem" else tok
                        cooked.extend([out_flag, str(resolved_inc).replace("\\", "/")])
                    else:
                        cooked.extend([tok, str(resolved_inc).replace("\\", "/")])
                else:
                    cooked.extend([tok, str(cand).replace("\\", "/")])
                i += 2
                continue
            if tok.startswith("-I") and len(tok) > 2:
                inc = tok[2:]
                cand = execroot / inc
                resolved_inc = cand.resolve() if cand.exists() else cand
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
    parser.add_argument("action", choices=("build", "compdb", "fetch", "clean"))
    parser.add_argument("targets", nargs="*")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    pixi_prefix = root / ".pixi/envs/default"
    bazel = find_bazel(pixi_prefix)

    repo_cache = root / "build/.bazel_repo_cache"
    disk_cache = root / "build/.bazel_disk_cache"
    repo_cache.mkdir(parents=True, exist_ok=True)
    disk_cache.mkdir(parents=True, exist_ok=True)
    seed_repo_cache_from_xmake(repo_cache)

    out_dir = (root / (args.build_directory or Path("build") / f"bazel-{args.profile}")).resolve()
    rev = engine_git_revision(root / "vendor/engrave")

    env = dict(os.environ)
    extra_paths = [pixi_prefix / "Library/bin", pixi_prefix / "bin"]
    env["PATH"] = os.pathsep.join(str(p) for p in extra_paths if p.is_dir()) + os.pathsep + env.get("PATH", "")

    common_flags = [
        f"--config={args.profile}",
        f"--repository_cache={repo_cache}",
        f"--disk_cache={disk_cache}",
        f"--repo_env=ENGRAVE_PIXI_PREFIX={pixi_prefix}",
        f"--repo_env=ENGRAVE_GIT_REVISION={rev}",
    ]
    if args.no_pch:
        common_flags.append("--@engrave//bazel:pch=false")

    build_flags = [
        *common_flags,
        f"--symlink_prefix={root / 'build' / f'.bazel-symlinks-{args.profile}'}/",
    ]
    if args.jobs:
        build_flags.append(f"--jobs={args.jobs}")

    if args.action == "clean":
        return subprocess.call([bazel, "clean"], cwd=root, env=env)

    labels = [resolve_target(t) for t in (args.targets or ["all_binaries"])]

    if args.action == "fetch":
        return subprocess.call([bazel, "fetch", *common_flags, *labels], cwd=root, env=env)

    if args.action == "compdb":
        compdb_flags = [f for f in build_flags if not f.startswith("--@engrave//bazel:pch")] + [
            "--@engrave//bazel:pch=false"
        ]
        return generate_compdb(bazel, root, env, compdb_flags, common_flags, labels, out_dir)

    # Check native includes boundary before building host targets.
    py = pixi_prefix / ("python.exe" if os.name == "nt" else "bin/python")
    if py.is_file():
        rc = subprocess.call(
            [str(py), str(root / "scripts/check_native_includes.py"), "--engine-source", str(root / "vendor/engrave")],
            cwd=root,
            env=env,
        )
        if rc != 0:
            return rc

    with tempfile.NamedTemporaryFile(dir=root / "build", prefix=".bep-", suffix=".json", delete=False) as tf:
        bep_path = Path(tf.name)
    try:
        rc = subprocess.call(
            [bazel, "build", *build_flags, f"--build_event_json_file={bep_path}", *labels],
            cwd=root,
            env=env,
        )
        if rc != 0:
            return rc
        return stage_bep_outputs(bep_path, out_dir)
    finally:
        bep_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
