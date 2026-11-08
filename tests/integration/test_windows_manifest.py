"""Check the application manifest embedded in Windows executables."""

import argparse
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mt", default="mt.exe", help="Windows SDK manifest tool")
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("this check requires Windows")
    with tempfile.TemporaryDirectory() as directory:
        manifest = Path(directory) / "embedded.manifest"
        for binary in args.binaries:
            subprocess.run(
                [args.mt, f"-inputresource:{binary.resolve()};#1", f"-out:{manifest}"],
                check=True,
                capture_output=True,
            )
            root = ET.parse(manifest).getroot()
            for setting, namespace, expected in (
                ("heapType", "2020", "SegmentHeap"),
                ("activeCodePage", "2019", "UTF-8"),
            ):
                element = root.find(f".//{{http://schemas.microsoft.com/SMI/{namespace}/WindowsSettings}}{setting}")
                if element is None or element.text != expected:
                    raise RuntimeError(f"{binary}: expected {setting}={expected}")
            privilege = root.find(".//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel")
            if privilege is None or privilege.get("level") != "asInvoker":
                raise RuntimeError(f"{binary}: expected asInvoker execution level")
            print(f"{binary}: SegmentHeap, UTF-8, asInvoker")


if __name__ == "__main__":
    main()
