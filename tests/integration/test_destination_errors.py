"""Destination metadata errors retain their filesystem classification."""

import argparse
import errno
import json
import os
import subprocess
from pathlib import Path

from support.paths import REPO_ROOT
from support.process_client import Client, export_args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--soundfont",
        type=Path,
        default=REPO_ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3",
    )
    a = parser.parse_args()
    a.output = a.output.resolve()
    c = Client(a.binary, a.resources, a.output, soundfont=a.soundfont)
    results = {}
    try:
        doc = c.tool("score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
            "document"
        ]
        measure = c.tool("score_outline", {"document": doc})["outline"]["measures"][0]["id"]
        c.tool(
            "score_edit",
            dict(
                document=doc,
                ops=[
                    dict(
                        op="set_layout_break",
                        measure=dict(kind="id", id=measure),
                        kind="page",
                        on=True,
                    )
                ],
            ),
        )

        def export(fmt, dest, ok=True):
            score = fmt in ("mscx", "mscz", "musicxml", "mxl", "midi")
            kind = "score" if score else ("audio" if fmt == "wav" else "render")
            args = dict(
                document=doc,
                kind=kind,
                options=dict(format=fmt),
                destination=dict(
                    kind="directory" if fmt in ("svg", "png") else "file",
                    path=str(dest),
                ),
            )
            return c.tool("score_export", args, ok)

        for fmt in (
            "mscx",
            "mscz",
            "musicxml",
            "mxl",
            "midi",
            "pdf",
            "svg",
            "png",
            "wav",
        ):
            target = a.output / "missing" / fmt / "nested"
            directory = fmt in ("svg", "png")
            dest = target if directory else target / f"out.{fmt}"
            result = export(fmt, dest)
            paths = [item["path"] for item in result["artifacts"]]
            expected = [(target / f"page-{n:04}.{fmt}").as_posix() for n in (1, 2)] if directory else [dest.as_posix()]
            assert paths == expected, result
            assert all(Path(p).is_file() and Path(p).stat().st_size for p in paths)
            assert not list(target.glob(".engrave-stage-*"))
            results[f"created_{fmt}"] = paths

        def check(label, target, code):
            errors = {}
            for fmt, dest in [("svg", target), ("pdf", target / "out.pdf")]:
                args = export_args(doc, fmt, a.output, True)
                args["destination"]["path"] = str(dest)  # Never resolve away a failing probe.
                error = c.tool("score_export", args, False)["error"]
                assert error["code"] == code, error
                assert not error.get("published_artifacts"), error
                assert str(dest) in error["message"], error
                errors[fmt] = error
            results[label] = errors

        if os.name == "nt":
            # Use raw strings: pathlib would discard empty path components.
            invalid_names = [
                "bad<name",
                "bad>name",
                "bad:name",
                'bad"name',
                "bad|name",
                "bad?name",
                "bad*name",
                "bad\x01name",
                "NUL",
                "COM1",
                "con.txt",
                "PRN.ext",
                "AUX",
                "LPT9.log",
                "COM\u00b9",
                "lpt\u00b2.log",
                "trailing.",
                "trailing ",
                "empty//component",
                "trailing/",
            ]
            for index, name in enumerate(invalid_names):
                for fmt in ("svg", "pdf"):
                    root = a.output / f"invalid-{index}-{fmt}"
                    for position, suffix in [
                        ("leaf", name),
                        ("parent", name + "/nested/out"),
                    ]:
                        dest = str(root / position / "missing") + "/" + suffix
                        args = export_args(doc, fmt, a.output, True)
                        args["destination"]["path"] = dest
                        error = c.tool("score_export", args, False)["error"]
                        assert error["code"] == "InvalidArgument", error
                        assert dest in error["message"], error
                        assert not error.get("published_artifacts"), error
                        assert not root.exists(), f"Invalid destination created parents: {dest}"
                        results[f"syntax_{index}_{fmt}_{position}"] = error

            # Device namespaces and incomplete UNC roots are never destinations.
            namespace_cases = [
                ("svg", r"\\.\engrave_mcp_missing_58311\child"),
                ("pdf", r"\\.\engrave_mcp_missing_58311\out.pdf"),
                ("svg", r"\\"),
                ("pdf", r"\\"),
                ("svg", r"\\localhost"),
                ("pdf", r"\\localhost"),
                ("svg", r"\\?\C:\engrave_mcp_missing_58311"),
                ("pdf", r"\\?\C:\engrave_mcp_missing_58311\out.pdf"),
            ]
            for index, (fmt, dest) in enumerate(namespace_cases):
                args = export_args(doc, fmt, a.output, True)
                args["destination"]["path"] = dest
                error = c.tool("score_export", args, False)["error"]
                assert error["code"] == "InvalidArgument", error
                assert dest in error["message"], error
                assert not error.get("published_artifacts"), error
                results[f"namespace_{index}"] = error

            # A long invalid path must not lose the reason or split a code point.
            # 13 + 4,082 + 1 = 4,096 characters, the schema's limit; the UTF-8
            # form is 16,342 bytes, so the 16 KiB diagnostic abbreviates the path
            # in the middle while retaining its ends and the reason.
            long_name = "\U0001f600" * 4082 + "?"
            long_dest = "long-invalid/" + long_name
            args = export_args(doc, "pdf", a.output, True)
            args["destination"]["path"] = long_dest
            error = c.tool("score_export", args, False)["error"]
            assert error["code"] == "InvalidArgument", error
            message = error["message"]
            assert "invalid Windows destination filename syntax" in message, message[:200]
            assert "\ufffd" not in message
            assert long_name[:8] in message and long_name[-8:] in message
            assert len(message.encode("utf-8")) <= 16384
            results["long_utf8_diagnostic"] = {
                "length": len(message.encode("utf-8")),
                "code": error["code"],
            }

        for fmt in ("svg", "pdf"):
            root = a.output / f"invalid-nul-{fmt}"
            for dest in ("", str(root / "missing") + "/bad\0name"):
                args = export_args(doc, fmt, a.output, True)
                args["destination"]["path"] = dest
                error = c.tool("score_export", args, False)["error"]
                assert error["code"] == "InvalidArgument", error
                assert "destination '" + dest + "'" in error["message"], error
                assert not error.get("published_artifacts") and not root.exists(), error
                results[f"decode_{fmt}_{bool(dest)}"] = error

        regular = a.output / "regular"
        regular.write_text("not a directory")
        check("non_directory", regular / "child", "InvalidArgument")
        for fmt, dest in [("svg", regular), ("pdf", a.output / "missing")]:
            error = export(fmt, dest, False)["error"]
            assert error["code"] == "InvalidArgument" and not error.get("published_artifacts"), error
            assert str(dest) in error["message"], error
            results[f"wrong_type_{fmt}"] = error
        blocked = a.output / "blocked"
        target = blocked / "existing"
        target.mkdir(parents=True)
        if os.name == "posix":
            blocked.chmod(0o600)
            try:
                try:
                    target.stat()
                except OSError as e:
                    assert e.errno == errno.EACCES, e
                else:
                    raise AssertionError("No-search setup must actually deny stat (run unprivileged)")
                check("no_search_existing", target, "Io")
                check("no_search_missing", target / "missing" / "nested", "Io")
            finally:
                blocked.chmod(0o700)
            assert list(target.iterdir()) == []
            target.chmod(0o500)
            try:
                check("unwritable_staging", target, "Io")
                check("unwritable_creation", target / "missing" / "nested", "Io")
            finally:
                target.chmod(0o700)
            assert list(target.iterdir()) == []
            loop = a.output / "loop"
            loop.symlink_to(loop.name, target_is_directory=True)
            check("symlink_loop", loop, "Io")
            check("name_too_long", a.output / ("x" * 300), "Io")
        elif os.name == "nt":
            # Deny metadata access only on our disposable subtree, then restore it.
            import csv

            row = next(
                csv.reader(subprocess.check_output(["whoami", "/user", "/fo", "csv", "/nh"], text=True).splitlines())
            )
            sid = row[1]

            def acl(*args):
                subprocess.run(
                    ["icacls", str(blocked), *args],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            try:
                acl("/deny", f"*{sid}:(OI)(CI)(RA)")
                try:
                    target.stat()
                except PermissionError:
                    check("access_denied_existing", target, "Io")
                    check("access_denied_missing", target / "missing" / "nested", "Io")
                else:
                    results["access_denied_existing"] = (
                        "not reproducible: stat succeeds with inherited deny READ_ATTRIBUTES; Windows traverse privilege can bypass search denial"
                    )
            finally:
                acl("/remove:d", f"*{sid}", "/T")
            assert list(target.iterdir()) == []
            try:
                acl("/deny", f"*{sid}:(OI)(CI)(AD)")
                probe = target / "denied-probe"
                try:
                    probe.mkdir()
                except PermissionError:
                    check("access_denied_creation", target / "missing" / "nested", "Io")
                else:
                    probe.rmdir()
                    results["access_denied_creation"] = (
                        "not reproducible: mkdir succeeds with inherited deny ADD_SUBDIRECTORY"
                    )
            finally:
                acl("/remove:d", f"*{sid}", "/T")
            assert list(target.iterdir()) == []

        # A real rename failure, without an injected fault or metadata refusal.
        conflict = a.output / "conflict"
        conflict.mkdir()
        (conflict / "page-0001.svg").mkdir()
        args = export_args(doc, "svg", conflict, True)
        error = c.tool("score_export", args, False)["error"]
        assert error["code"] == "Io" and not error.get("published_artifacts"), error
        assert str(conflict) in error["message"], error
        assert sorted(p.name for p in conflict.iterdir()) == ["page-0001.svg"]
        results["publication_conflict"] = error
        # Recursive creation succeeds, then page two fails naturally at rename.
        # Page one is reported and staging is removed; created parents remain.
        partial = a.output / "created_partial" / "nested"
        export("pdf", partial / "initial.pdf")
        (partial / "page-0002.svg").mkdir()
        error = export("svg", partial, False)["error"]
        assert error["code"] == "Io", error
        assert error["published_artifacts"] == [(partial / "page-0001.svg").as_posix()], error
        assert sorted(p.name for p in partial.iterdir()) == [
            "initial.pdf",
            "page-0001.svg",
            "page-0002.svg",
        ]
        results["partial_after_creation"] = error
        assert "tools" in c.rpc("tools/list", {})["result"]
    finally:
        c.close()
    (a.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results, ensure_ascii=True))


if __name__ == "__main__":
    main()
