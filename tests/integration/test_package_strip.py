"""Check native staging, symbol retention, and runtime startup using a built release."""

import argparse
import asyncio
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pe_metadata(binary: Path) -> tuple[int, bytes, bytes, list[str]]:
    data = binary.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    coff = pe + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    symbols, optional_size = struct.unpack_from("<IH", data, coff + 12)
    opt_off = coff + 20
    magic = struct.unpack_from("<H", data, opt_off)[0]
    imp_dir_off = opt_off + (120 if magic == 0x20B else 104)
    imp_rva, imp_size = struct.unpack_from("<II", data, imp_dir_off)
    section_table = opt_off + optional_size
    sections: list[tuple[bytes, int, int, int, int]] = []
    for index in range(num_sections):
        offset = section_table + 40 * index
        name = data[offset : offset + 8].rstrip(b"\0")
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((name, vsize, va, rsize, rptr))

    def rva_to_off(rva: int) -> int:
        for _, vsize, va, rsize, rptr in sections:
            span = max(vsize, rsize)
            if va <= rva < va + span:
                return rptr + (rva - va)
        return 0

    pdata = b""
    xdata = b""
    for name, _, _, rsize, rptr in sections:
        if name == b".pdata":
            pdata = data[rptr : rptr + rsize]
        elif name == b".xdata":
            xdata = data[rptr : rptr + rsize]
    if not pdata:
        raise AssertionError(f"Missing .pdata in {binary}")

    # When clang/lld places x64 UNWIND_INFO in .rdata instead of a separate .xdata
    # section, hash every UNWIND_INFO record referenced by .pdata.
    if not xdata:
        unwind_hash = hashlib.sha256()
        for entry_off in range(0, len(pdata) - (len(pdata) % 12), 12):
            _, _, unwind_rva = struct.unpack_from("<III", pdata, entry_off)
            if unwind_rva == 0:
                continue
            off = rva_to_off(unwind_rva & ~1)
            if not off or off + 4 > len(data):
                raise AssertionError(f"Invalid UNWIND_INFO RVA {unwind_rva:#x} in {binary}")
            flags = data[off] >> 3
            code_count = data[off + 2]
            padded = (code_count + 1) & ~1
            size = 4 + 2 * padded
            if flags & 0x4:  # UNW_FLAG_CHAININFO
                size += 12
            elif flags & 0x3:  # UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
                size += 4
            unwind_hash.update(data[off : off + size])
        xdata = unwind_hash.digest()

    imports: list[str] = []
    if imp_rva and imp_size:
        off = rva_to_off(imp_rva)
        while off and off + 20 <= len(data):
            _, _, _, name_rva, _ = struct.unpack_from("<IIIII", data, off)
            if not name_rva:
                break
            noff = rva_to_off(name_rva)
            if not noff:
                break
            end = data.index(0, noff)
            imports.append(data[noff:end].decode("ascii", errors="replace").lower())
            off += 20

    return symbols, pdata, xdata, sorted(imports)


def elf_sections(binary: Path) -> set[str]:
    output = subprocess.check_output(["readelf", "-SW", str(binary)], text=True)
    return set(re.findall(r"^\s*\[\s*\d+\]\s+(\.\S+)", output, re.MULTILINE))


def check_package(packager: Path, source: Path, directory: Path, no_strip: bool, source_sha256: str) -> Path:
    windows = sys.platform == "win32"
    command = [sys.executable, str(packager), "--binary", str(source), "--output", str(directory)]
    if no_strip:
        command.append("--no-strip")
    soundfont = REPO_ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"
    if soundfont.is_file():
        command.extend(["--soundfont", str(soundfont)])
    subprocess.run(command, check=True)

    licenses = directory / "licenses"
    assert (licenses / "freetype/FTL.TXT").is_file()
    assert (licenses / "engine-THIRD_PARTY_NOTICES.md").is_file()
    for path in licenses.rglob("*"):
        rel = path.relative_to(licenses).as_posix().lower()
        assert path.name.lower() != "gplv2.txt" and "fdk-aac" not in rel, path
    if soundfont.is_file():
        assert sha256(directory / "soundfonts" / soundfont.name) == sha256(soundfont)

    executable = directory / ("engrave-mcp.exe" if windows else "engrave-mcp")
    if windows:
        src_meta = pe_metadata(source)
        symbols, pdata, xdata, imports = pe_metadata(executable)
        assert pdata and pdata == src_meta[1]
        assert xdata and xdata == src_meta[2]
        assert imports and imports == src_meta[3]
        lower_imports = {name.lower() for name in imports}
        assert {"cairo.dll", "freetype.dll", "zlib.dll"} <= lower_imports, imports
        if no_strip:
            assert symbols > 0 and sha256(executable) == source_sha256
        else:
            assert symbols == 0
            manifest_check = REPO_ROOT / "tests/integration/test_windows_manifest.py"
            subprocess.run([sys.executable, str(manifest_check), str(executable)], check=True)
    else:
        sections = elf_sections(executable)
        assert {".eh_frame", ".dynsym", ".dynstr"} <= sections
        if no_strip:
            assert {".symtab", ".strtab"} <= sections
            syms = subprocess.check_output(["readelf", "--syms", "-W", str(executable)], text=True)
            assert not re.search(
                r"^\s*\d+:\s+[0-9a-f]+\s+\d+\s+FUNC\s+\S+\s+\S+\s+\d+\s+FT_Init_FreeType\b", syms, re.M
            )
        else:
            assert not {".symtab", ".strtab"} & sections
        dynamic = subprocess.check_output(["readelf", "-dW", str(executable)], text=True)
        assert "[$ORIGIN/lib]" in dynamic
        assert "[libfreetype.so.6]" in dynamic
        dynsyms = subprocess.check_output(["readelf", "--dyn-syms", "-W", str(executable)], text=True)
        assert re.search(r"\bUND\s+FT_Init_FreeType\b", dynsyms), "FT_Init_FreeType must be imported dynamically"

    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["unstripped_executable_sha256"] == source_sha256
    assert manifest["files"][executable.name] == {
        "bytes": executable.stat().st_size,
        "sha256": sha256(executable),
    }
    assert sha256(source) == source_sha256
    return executable


async def run_basic_startup_check(binary: Path, workdir: Path) -> None:
    if sys.platform == "win32":
        child_env = {
            "SystemRoot": os.environ["SystemRoot"],
            "PATH": str(Path(os.environ["SystemRoot"]) / "System32"),
            "TEMP": os.environ["TEMP"],
            "TMP": os.environ["TMP"],
        }
    else:
        child_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
    params = StdioServerParameters(command=str(binary), args=[], env=child_env, cwd=str(workdir))
    with (workdir / "startup-stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                assert initialized.server_info.name == "engrave-mcp"
                info = await session.call_tool("runtime_info", {})
                assert not info.is_error and info.structured_content["ok"]


def main() -> None:
    windows = sys.platform == "win32"
    executable = "engrave-mcp.exe" if windows else "engrave-mcp"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO_ROOT / "build/release" / executable)
    parser.add_argument(
        "--packager",
        type=Path,
        default=REPO_ROOT / "scripts" / ("package_windows.py" if windows else "package_linux.py"),
    )
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "notes/runs")
    args = parser.parse_args()
    source = args.binary.resolve()
    packager = args.packager.resolve()
    source_sha256 = sha256(source)

    output_base = args.output.resolve()
    output_base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-strip-", dir=output_base) as directory:
        root = Path(directory)
        stripped = check_package(packager, source, root / "stripped", False, source_sha256)
        unstripped = check_package(packager, source, root / "unstripped", True, source_sha256)
        assert stripped.stat().st_size < unstripped.stat().st_size

        soundfont = stripped.parent / "soundfonts/FluidR3Mono_GM.sf3"
        if soundfont.is_file():
            environment = dict(
                os.environ,
                ENGRAVE_MCP_BINARY=str(stripped),
                ENGRAVE_MCP_SOUNDFONT=str(soundfont),
                ENGRAVE_MCP_OUTPUT=str(root / "smoke"),
                ENGRAVE_MCP_CLEAN_ENV="1",
            )
            environment.pop("ENGRAVE_MCP_RESOURCES", None)
            smoke = REPO_ROOT / "tests/integration/test_mcp_smoke.py"
            subprocess.run([sys.executable, str(smoke)], check=True, env=environment)
        else:
            workdir = root / "startup"
            workdir.mkdir(parents=True, exist_ok=True)
            asyncio.run(run_basic_startup_check(stripped, workdir))


if __name__ == "__main__":
    main()
