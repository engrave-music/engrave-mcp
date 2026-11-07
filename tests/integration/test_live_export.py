"""Live routing, revisions, cancellation, atomic publication, multi-page exports.

Use --binary engrave-mcp-test for the deterministic owner-queue cancellation seam.
This is the complete entry point: --direct-binary is required. The native
--partial-write mode returns 3 and leaves PDF/MSCZ comparison to this wrapper.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import zipfile
from pathlib import Path

from support.process_client import ROOT, Client, export_args


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--resources", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--soundfont", type=Path, required=True)
    p.add_argument(
        "--direct-binary",
        type=Path,
        required=True,
        help="native partial writer; this wrapper completes its comparisons",
    )
    a = p.parse_args()
    a.output = a.output.resolve()
    # A bare invocation must fail closed, before touching resources/output.
    bare = subprocess.run(
        [
            str(a.direct_binary.resolve()),
            str(a.resources.resolve()),
            str(a.output / "bare"),
        ],
        capture_output=True,
        text=True,
    )
    if bare.returncode == 0 or "partial: no content comparison" not in bare.stderr:
        raise RuntimeError("native standalone invocation did not fail closed")
    oracle = a.output / "direct"
    partial = subprocess.run(
        [
            str(a.direct_binary.resolve()),
            "--partial-write",
            str(a.resources.resolve()),
            str(oracle),
            str(ROOT / "vendor/engrave/tests/data"),
            str(ROOT / "tests/data/live_export/created.musicxml"),
        ],
        capture_output=True,
        text=True,
    )
    if partial.returncode != 3 or "partial: no content comparison" not in partial.stdout:
        raise RuntimeError(f"partial writer contract failed: {partial.returncode} {partial.stdout} {partial.stderr}")
    from support.golden import describe_pdf

    folders = [folder for folder in oracle.iterdir() if folder.is_dir()]
    if not folders:
        raise RuntimeError("empty direct oracle")
    for folder in folders:
        if describe_pdf(folder / "direct.pdf") != describe_pdf(folder / "pdf"):
            raise RuntimeError(f"PDF content mismatch: {folder}")
        with (
            zipfile.ZipFile(folder / "direct.mscz") as left,
            zipfile.ZipFile(folder / "mscz") as right,
        ):
            if sorted(left.namelist()) != sorted(right.namelist()):
                raise RuntimeError(f"MSCZ member mismatch: {folder}")
            for name in left.namelist():
                if left.read(name) != right.read(name):
                    raise RuntimeError(f"MSCZ content mismatch: {folder}/{name}")
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/integration/test_live_publication_cancel.py"),
            "--binary",
            str(a.binary),
            "--resources",
            str(a.resources),
            "--output",
            str(a.output / "publication-cancel"),
        ],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/integration/test_live_publication_limits.py"),
            "--binary",
            str(a.binary),
            "--resources",
            str(a.resources),
            "--output",
            str(a.output / "publication-limits"),
        ],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/integration/test_destination_errors.py"),
            "--binary",
            str(a.binary),
            "--resources",
            str(a.resources),
            "--output",
            str(a.output / "destination-errors"),
            "--soundfont",
            str(a.soundfont.resolve()),
        ],
        check=True,
    )
    results = {}
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/integration/test_live_path_utf8.py"),
            "--binary",
            str(a.binary),
            "--resources",
            str(a.resources),
            "--output",
            str(a.output / "path-utf8"),
        ],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/integration/test_live_report_delivery.py"),
            "--binary",
            str(a.binary),
            "--resources",
            str(a.resources),
            "--output",
            str(a.output / "report-delivery"),
        ],
        check=True,
    )
    for mode in (None,):
        out = a.output / (mode or "default")
        c = Client(a.binary, a.resources, out, mode)
        doc = c.tool("score_open", {"path": str(ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
            "document"
        ]
        for fmt, dest in [
            ("pdf", out / "missing-parent" / "out.pdf"),
            ("svg", out / "missing-directory"),
        ]:
            created = c.tool("score_export", export_args(doc, fmt, dest))
            assert all(Path(item["path"]).is_file() for item in created["artifacts"]), created
        hashes = {}
        for fmt in ("mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "svg", "png"):
            path = out / fmt
            if fmt in ("svg", "png"):
                path.mkdir(exist_ok=True)
            args = export_args(doc, "svg" if fmt == "png" else fmt, path)
            args["options"]["format"] = fmt
            if fmt in ("mscx", "musicxml", "mxl", "midi"):
                args["kind"] = "score"
                args["options"] = {"format": fmt}
            r = c.tool("score_export", args)
            artifact = Path(r["artifacts"][0]["path"])
            content = artifact.read_bytes()
            if fmt in ("mscz", "mxl"):
                with zipfile.ZipFile(artifact) as archive:
                    content = b"".join(name.encode() + archive.read(name) for name in sorted(archive.namelist()))
            if fmt == "pdf":
                content = re.sub(
                    rb"/(CreationDate|ModDate) \(D:[^)]*\)",
                    rb"/\1 (D:normalized)",
                    content,
                )
            hashes[fmt] = hashlib.sha256(content).hexdigest()
            args["expected_revision"] = "999"
            assert c.tool("score_export", args, False)["error"]["code"] == "RevisionConflict"
        refused = export_args(doc, "mscz", out / "refused.mscz")
        refused["options"]["native_version"] = "4.90"
        failure = c.tool("score_export", refused, False)
        assert failure["error"]["code"] == "Unsupported", failure
        assert "export_warnings" in failure["error"]
        assert not (out / "refused.mscz").exists()
        positions = out / "positions.json"
        c.tool(
            "score_export",
            dict(
                document=doc,
                kind="positions",
                options={"position_kind": "measure"},
                destination={"kind": "file", "path": str(positions)},
            ),
        )
        hashes["positions"] = hashlib.sha256(positions.read_bytes()).hexdigest()
        outline = c.tool("score_outline", {"document": doc})["outline"]
        measure = outline["measures"][0]["id"]
        c.tool(
            "score_edit",
            {
                "document": doc,
                "ops": [
                    {
                        "op": "set_layout_break",
                        "measure": {"kind": "id", "id": measure},
                        "kind": "page",
                        "on": True,
                    }
                ],
            },
        )
        pages = out / "pages"
        pages.mkdir(exist_ok=True)
        r = c.tool("score_export", export_args(doc, "svg", pages, True))
        assert len(r["artifacts"]) >= 2
        # Failure on page 2 preserves page 1 and reports it, no staged leftovers.
        partial = out / "partial"
        partial.mkdir(exist_ok=True)
        (partial / "page-0002.svg").mkdir(exist_ok=True)
        r = c.tool("score_export", export_args(doc, "svg", partial, True), False)
        assert r["error"]["code"] == "Io", r
        assert r["error"]["published_artifacts"] == [str((partial / "page-0001.svg").resolve()).replace("\\", "/")], r
        assert not list(partial.glob(".engrave-stage-*"))
        c.tool("score_export", export_args(doc, "pdf", out / "multi.pdf", True))
        # Queue an export behind a known running owner task, cancel before engine use.
        sleeper = c.send("tools/call", {"name": "test_owner_sleep", "arguments": {}})
        time.sleep(0.05)
        dest = out / "cancel.pdf"
        ident = c.send(
            "tools/call",
            {"name": "score_export", "arguments": export_args(doc, "pdf", dest)},
        )
        c.send("notifications/cancelled", {"requestId": ident}, True)
        c.receive(sleeper)
        assert c.receive(ident)["error"]["code"] == -32800
        assert not dest.exists()
        phases = c.close()
        modes = {x["mode"] for x in phases}
        assert modes == {"live"}, modes
        results[mode or "default"] = hashes
    # Test-only binary: fail the shared pre-rename flush in both callers.
    for mode in ("live",):
        for seam in ("FILE_FLUSH", "STAGING") if mode == "live" else ("FILE_FLUSH",):
            key = "ENGRAVE_MCP_TEST_FAIL_" + seam
            os.environ[key] = "1"
            try:
                c = Client(a.binary, a.resources, a.output / (mode + "-" + seam), mode)
            finally:
                del os.environ[key]
            doc = c.tool("score_open", {"path": str(ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
                "document"
            ]
            dest = a.output / (mode + "-" + seam) / "old.pdf"
            for existing in (False, True):
                if existing:
                    dest.write_bytes(b"old destination")
                else:
                    dest.unlink(missing_ok=True)
                failure = c.tool("score_export", export_args(doc, "pdf", dest), False)
                assert failure["error"]["code"] == "Io", failure
                assert not failure["error"].get("published_artifacts"), failure
                assert dest.read_bytes() == b"old destination" if existing else not dest.exists()
                assert not list(dest.parent.glob(".engrave-stage-*"))
            c.close()
    (a.output / "comparison.json").write_text(json.dumps(results, indent=2))
    (a.output / "comparison.json").write_text(json.dumps(results, indent=2))
    print(
        f"complete: live export passed; {len(folders)} PDF/MSCZ pairs compared; render hash differences retained in comparison.json"
    )


if __name__ == "__main__":
    main()
