"""Check the application manifest embedded in Windows executables.

Windows applies `heapType` and `activeCodePage` only under
assembly/application/windowsSettings, so the check requires that exact path.
With `--build-dir` it checks the product and representative test executables;
`--self-test` runs the negative controls without any binary.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ASM_V1 = "urn:schemas-microsoft-com:asm.v1"
ASM_V3 = "urn:schemas-microsoft-com:asm.v3"
SETTINGS = (
    ("heapType", "http://schemas.microsoft.com/SMI/2020/WindowsSettings", "SegmentHeap"),
    ("activeCodePage", "http://schemas.microsoft.com/SMI/2019/WindowsSettings", "UTF-8"),
)
# The product and the engine test binary that measures audio teardown.
REPRESENTATIVE = ("engrave-mcp", "engrave-mcp-test", "engrave_output_test")


def manifest_errors(root: ET.Element) -> list[str]:
    errors = []
    if root.tag != f"{{{ASM_V1}}}assembly":
        return [f"root element is {root.tag}, not asm.v1 assembly"]
    settings = root.find(f"{{{ASM_V3}}}application/{{{ASM_V3}}}windowsSettings")
    for name, namespace, expected in SETTINGS:
        element = None if settings is None else settings.find(f"{{{namespace}}}{name}")
        if element is None or (element.text or "").strip() != expected:
            errors.append(f"expected application/windowsSettings/{name}={expected}")
    privilege = root.find(f".//{{{ASM_V3}}}requestedExecutionLevel")
    if privilege is None or privilege.get("level") != "asInvoker":
        errors.append("expected asInvoker execution level")
    return errors


def find_mt(explicit: str | None) -> str:
    if explicit:
        return explicit
    found = shutil.which("mt.exe")
    if found:
        return found
    kits = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    candidates = sorted(glob.glob(os.path.join(kits, "Windows Kits", "10", "bin", "*", "x64", "mt.exe")))
    if not candidates:
        raise RuntimeError("mt.exe not found; install the Windows SDK or pass --mt")
    return candidates[-1]


def check_binary(mt: str, binary: Path, directory: Path) -> list[str]:
    if not binary.is_file():
        return ["binary does not exist"]
    manifest = directory / "embedded.manifest"
    result = subprocess.run(
        [mt, "-nologo", f"-inputresource:{binary.resolve()};#1", f"-out:{manifest}"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return [f"no embedded manifest ({result.stdout.strip() or result.stderr.strip()})"]
    return manifest_errors(ET.parse(manifest).getroot())


def self_test() -> None:
    good = f"""<assembly xmlns="{ASM_V1}" manifestVersion="1.0">
  <trustInfo xmlns="{ASM_V3}"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <application xmlns="{ASM_V3}"><windowsSettings>
    <activeCodePage xmlns="{SETTINGS[1][1]}">UTF-8</activeCodePage>
    <heapType xmlns="{SETTINGS[0][1]}">SegmentHeap</heapType>
  </windowsSettings></application>
</assembly>"""
    assert manifest_errors(ET.fromstring(good)) == [], "valid manifest rejected"
    # Windows ignores heapType outside windowsSettings; the check must too.
    misplaced = good.replace(
        f'<heapType xmlns="{SETTINGS[0][1]}">SegmentHeap</heapType>\n  </windowsSettings>',
        f'</windowsSettings><heapType xmlns="{SETTINGS[0][1]}">SegmentHeap</heapType>',
    )
    assert misplaced != good
    assert manifest_errors(ET.fromstring(misplaced)), "misplaced heapType accepted"
    wrong_value = good.replace(">SegmentHeap<", ">NtHeap<")
    assert manifest_errors(ET.fromstring(wrong_value)), "wrong heapType accepted"
    missing = good.replace(f'<heapType xmlns="{SETTINGS[0][1]}">SegmentHeap</heapType>', "")
    assert manifest_errors(ET.fromstring(missing)), "missing heapType accepted"
    print("self-test: valid accepted; misplaced, wrong and missing heapType rejected")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mt", help="Windows SDK manifest tool (default: PATH, then the newest Windows SDK)")
    parser.add_argument("--build-dir", type=Path, help="check the representative executables in this build dir")
    parser.add_argument("--self-test", action="store_true", help="run the negative controls only")
    parser.add_argument("binaries", nargs="*", type=Path)
    args = parser.parse_args()
    self_test()
    if args.self_test:
        return 0
    if sys.platform != "win32":
        print("skipped: embedded manifests exist only on Windows")
        return 0
    binaries = list(args.binaries)
    if args.build_dir:
        binaries += [args.build_dir / f"{name}.exe" for name in REPRESENTATIVE]
    if not binaries:
        parser.error("pass --build-dir or binaries")
    mt = find_mt(args.mt)
    failed = False
    with tempfile.TemporaryDirectory() as directory:
        for binary in binaries:
            errors = check_binary(mt, binary, Path(directory))
            if errors:
                failed = True
                for error in errors:
                    print(f"{binary}: {error}")
            else:
                print(f"{binary}: SegmentHeap, UTF-8, asInvoker")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
