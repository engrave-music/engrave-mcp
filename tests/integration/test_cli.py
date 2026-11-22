"""Verify CLI flags (--help, --version, invalid flags) and sibling SoundFont auto-discovery."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import shutil
import subprocess
import tempfile
import tomllib
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_testing.paths import pixi_env
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def expected_host_version() -> str:
    header = (REPO_ROOT / "engrave_mcp/version.h").read_text(encoding="utf-8")
    header_match = re.search(r'\bhost_version\s*=\s*"([^"]+)"', header)
    assert header_match, "missing host_version in engrave_mcp/version.h"
    version = header_match.group(1)

    module_text = (REPO_ROOT / "MODULE.bazel").read_text(encoding="utf-8")
    module_match = re.search(r'\bmodule\s*\([^)]*?\bversion\s*=\s*"([^"]+)"', module_text, re.DOTALL)
    assert module_match and module_match.group(1) == version

    pixi_data = tomllib.loads((REPO_ROOT / "pixi.toml").read_text(encoding="utf-8"))
    assert pixi_data["workspace"]["version"] == version
    return version


def server_env() -> dict[str, str]:
    env = dict(os.environ)
    prefix = pixi_env(REPO_ROOT)
    if os.name == "nt":
        env["PATH"] = str(prefix / "Library/bin") + os.pathsep + env.get("PATH", "")
    elif (prefix / "lib").is_dir():
        env["LD_LIBRARY_PATH"] = str(prefix / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def run_cli(binary: Path, args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *args],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )


async def probe_server(
    binary: Path,
    args: list[str],
    env: dict[str, str],
    errlog_path: Path,
) -> tuple[str, list[tuple[Path, bytes]]]:
    params = StdioServerParameters(command=str(binary), args=args, env=env, cwd=str(errlog_path.parent))
    with errlog_path.open("w", encoding="utf-8") as errlog:
        async with stdio_client(params, errlog=errlog) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                assert initialized.server_info.name == "engrave-mcp", initialized.server_info
                info = await session.call_tool("runtime_info", {})
                assert not info.is_error, info
                payload = info.structured_content
                assert payload and payload["ok"], payload
                soundfonts = [(Path(p), Path(p).read_bytes()) for p in payload["capabilities"]["soundfonts"]]
                return initialized.server_info.version, soundfonts


async def run_tests(args: argparse.Namespace) -> None:
    binary = args.binary.resolve()
    resources = args.resources.resolve()
    soundfont = args.soundfont.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = server_env()
    version = expected_host_version()

    for flag in ("--help", "-h"):
        res = run_cli(binary, [flag], env)
        assert res.returncode == 0, (flag, res.returncode, res.stderr)
        assert res.stderr == "", (flag, res.stderr)
        assert "Usage: engrave-mcp" in res.stdout, res.stdout
        for option in ("--soundfont", "--resources", "--help", "--version"):
            assert option in res.stdout, (flag, option, res.stdout)

    for flag in ("--version", "-V"):
        res = run_cli(binary, [flag], env)
        assert res.returncode == 0, (flag, res.returncode, res.stderr)
        assert res.stderr == "", (flag, res.stderr)
        assert res.stdout.strip() == f"engrave-mcp {version}", (flag, res.stdout)

    for bad_args in (["--unknown-flag"], ["--soundfont"], ["--resources"], ["unexpected"]):
        res = run_cli(binary, bad_args, env)
        assert res.returncode == 1, (bad_args, res.returncode)
        assert res.stdout == "", (bad_args, res.stdout)
        assert "Usage:" in res.stderr, (bad_args, res.stderr)

    with tempfile.TemporaryDirectory(prefix="cli-") as tmp_dir:
        bundle = Path(tmp_dir)
        bundled_exe = bundle / binary.name
        shutil.copy2(binary, bundled_exe)

        server_ver, loaded_none = await probe_server(
            bundled_exe,
            ["--resources", str(resources)],
            env,
            output / "no-soundfont.stderr.log",
        )
        assert server_ver == version, (server_ver, version)
        assert loaded_none == [], loaded_none

        sibling_sf = bundle / "soundfonts" / "FluidR3Mono_GM.sf3"
        sibling_sf.parent.mkdir(parents=True)
        shutil.copy2(soundfont, sibling_sf)

        server_ver, loaded_auto = await probe_server(
            bundled_exe,
            ["--resources", str(resources)],
            env,
            output / "auto-soundfont.stderr.log",
        )
        assert server_ver == version
        assert len(loaded_auto) == 1, loaded_auto
        assert loaded_auto[0][0].name == "FluidR3Mono_GM.sf3"
        assert loaded_auto[0][1] == sibling_sf.read_bytes()

        # Replace sibling with an invalid payload to verify explicit --soundfont
        # suppresses auto-discovery rather than loading the sibling file.
        sibling_sf.write_bytes(b"invalid-sibling-soundfont")
        server_ver, loaded_explicit = await probe_server(
            bundled_exe,
            ["--resources", str(resources), "--soundfont", str(soundfont)],
            env,
            output / "explicit-soundfont.stderr.log",
        )
        assert server_ver == version
        assert len(loaded_explicit) == 1, loaded_explicit
        assert loaded_explicit[0][1] == soundfont.read_bytes()

        # Directory at sibling path must not be treated as a SoundFont file.
        sibling_sf.unlink()
        sibling_sf.mkdir()
        server_ver, loaded_dir = await probe_server(
            bundled_exe,
            ["--resources", str(resources)],
            env,
            output / "dir-soundfont.stderr.log",
        )
        assert server_ver == version
        assert loaded_dir == [], loaded_dir

    report = {
        "status": "passed",
        "version": version,
        "help_flags": ["--help", "-h"],
        "version_flags": ["--version", "-V"],
        "invalid_cases": 4,
        "soundfont_auto_discovery": True,
        "soundfont_explicit_override": True,
    }
    (output / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


def main() -> int:
    default_binary = REPO_ROOT / ("build/release/engrave-mcp.exe" if os.name == "nt" else "build/release/engrave-mcp")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path(os.environ.get("ENGRAVE_MCP_BINARY", default_binary)),
    )
    parser.add_argument(
        "--resources",
        type=Path,
        default=Path(os.environ.get("ENGRAVE_MCP_RESOURCES", REPO_ROOT / ".runtime/assets")),
    )
    parser.add_argument(
        "--soundfont",
        type=Path,
        default=Path(
            os.environ.get(
                "ENGRAVE_MCP_SOUNDFONT",
                REPO_ROOT / "vendor/engrave/tests/data/engine_audio/sine.sf2",
            )
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(os.environ.get("ENGRAVE_MCP_OUTPUT", REPO_ROOT / "notes/runs/cli")),
    )
    args = parser.parse_args()
    asyncio.run(asyncio.wait_for(run_tests(args), timeout=60))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
