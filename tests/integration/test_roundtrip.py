"""Round-trip verification suite (roadmap step 0).

Exercises document creation, editing, file imports and excerpts through native
MCP tools, verifying semantic MSCX invariance under MSCZ round trips and
MusicXML subset parity against known interop losses.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import sys
import time
from fractions import Fraction
from pathlib import Path
from typing import Any

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_testing.mscx import (
    compare_mscz_xml,
    parse_mscx,
)
from engrave_testing.paths import pixi_env
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

# Known interop losses observed at the pre-fork baseline.
# {fixture: {phase: (digest, justification)}}; phase is "mscz_text", "musicxml", or "idempotence".
# A different difference list fails with its new digest; an entry whose phase no longer differs is reported stale.
KNOWN_LOSSES: dict[str, dict[str, tuple[str, str]]] = {
    "bend.gp": {
        "musicxml": (
            "f386eae23b9c8cb7",
            "The same GP grace-after notes become grace-before on the following chord; the corrected 5.00 parser changes the digest representation.",
        ),
    },
    "drums_import.musicxml": {
        "mscz_text": (
            "77d7cc79933f0666",
            "The same title size 24 is synthesized on reopen as in v1 (text digest 3a759cb7769875f9); this digest pins its v2 XML location and value.",
        ),
    },
    "custom_part_name.mscx": {
        "musicxml": ("05dd75d215fedcc1", "invisible fff dynamic is not exported"),
    },
    "hairpins_and_repeats.mscx": {
        "musicxml": ("05dd75d215fedcc1", "invisible fff dynamic is not exported"),
    },
    "layout_elements.mscx": {
        "musicxml": (
            "4a1004c05107bbb7",
            "HairPin beginText exported as dynamic mf; harmony moved to staff 1",
        ),
        "idempotence": (
            "aeab1d528f30d872",
            "unrendered MusicXML harmony dropped on second export",
        ),
    },
    "linked_parts.mscx": {
        "musicxml": (
            "fb3d4f6354501cb7",
            "The same grace conversion and backward cross-staff tie occur in historical 4.7.6 A/C files; the corrected 5.00 parser describes them differently.",
        ),
        "idempotence": (
            "efa1ff9698e4b076",
            "the tie re-attached to moved grace note is dropped on second import",
        ),
    },
    "linked_parts.mscx:excerpt:0": {
        "mscz_text": (
            "ed93cce937e37df7",
            "Standalone excerpt reopen loses only the root name Flute; the historical v1 text digest 77d073cac18ac443 also covered lost style and links, which v2 preserves.",
        ),
        "musicxml": (
            "fb3d4f6354501cb7",
            "Same grace conversion and backward cross-staff tie as the linked master; parser representation changed.",
        ),
        "idempotence": (
            "efa1ff9698e4b076",
            "the tie re-attached to moved grace note is dropped on second import",
        ),
    },
    "slurs.mscz": {
        "musicxml": (
            "02ce304f5126a1fd",
            "grace16after slurs: grace-after notes converted to grace-before in MusicXML",
        ),
    },
    "ghost_note.mscx": {
        "musicxml": (
            "7872bae4c4cf03e9",
            "linked tab staff gains an explicit 4/4 time signature after MusicXML import",
        ),
    },
    "tab_grace_prebend.mscx": {
        "musicxml": (
            "7872bae4c4cf03e9",
            "linked tab staff gains an explicit 4/4 time signature after MusicXML import",
        ),
    },
}

ALLOWED_SPANNERS = {"Slur", "HairPin", "Volta", "Pedal", "Ottava", "Tie"}


# The archive writer supplies these A4 defaults when a score is saved and
# reopened.  Only this complete, ordered insertion, at a Score's own Style,
# is normalized; an existing or different page setting remains observable.


def to_musicxml_subset(sem: dict[str, Any]) -> dict[str, Any]:
    """Project semantic representation down to MusicXML-comparable subset."""
    meta = {
        "title": sem["metadata"].get("workTitle", "") or sem["metadata"].get("movementTitle", ""),
        "composer": sem["metadata"].get("composer", ""),
    }
    parts = [{"name": p["longName"] or p["trackName"], "staffCount": p["staffCount"]} for p in sem["parts"]]
    measures = {
        sid: {
            "count": len(mlist),
            "measures": [
                {
                    "timesig": m["timesig"],
                    "startRepeat": m["startRepeat"],
                    "endRepeat": m["endRepeat"],
                }
                for m in mlist
            ],
        }
        for sid, mlist in sem["measures"].items()
    }
    events = {
        sid: {
            vid: [
                (
                    kind,
                    tick,
                    dur,
                    tuple((p, tie) for p, tpc, tie, *rest in notes),
                    grace_kind,
                )
                for kind, tick, dur, notes, grace_kind, *rest in elist
            ]
            for vid, elist in vmap.items()
        }
        for sid, vmap in sem["events"].items()
    }
    spanners = {
        sid: [(sp[0], sp[1], sp[2], sp[3]) for sp in splist if sp[0] in ALLOWED_SPANNERS]
        for sid, splist in sem["spanners"].items()
    }
    annotations = {
        sid: {
            "dynamics": ann["dynamics"],
            "tempos": ann["tempos"],
            "lyrics": ann["lyrics"],
            "harmony": ann["harmony"],
        }
        for sid, ann in sem["annotations"].items()
    }
    return {
        "metadata": meta,
        "parts": parts,
        "measures": measures,
        "events": events,
        "spanners": spanners,
        "annotations": annotations,
    }


def diff_data(a: Any, b: Any, path: str = "") -> list[tuple[str, Any, Any]]:
    """Recursively collect differences between structured objects."""
    diffs = []
    if type(a) is not type(b):
        return [(path, a, b)]
    if isinstance(a, dict):
        all_keys = sorted(set(a.keys()) | set(b.keys()))
        for k in all_keys:
            sub = f"{path}.{k}" if path else str(k)
            if k not in a:
                diffs.append((sub, None, b[k]))
            elif k not in b:
                diffs.append((sub, a[k], None))
            else:
                diffs.extend(diff_data(a[k], b[k], sub))
    elif isinstance(a, list):
        if len(a) != len(b):
            diffs.append((f"{path}.length", len(a), len(b)))
        for idx in range(min(len(a), len(b))):
            diffs.extend(diff_data(a[idx], b[idx], f"{path}[{idx}]"))
    elif a != b:
        diffs.append((path, a, b))
    return diffs


class RoundTripRunner:
    def __init__(
        self,
        session: ClientSession,
        output_dir: Path,
        self_test: bool = False,
        no_losses: bool = False,
    ):
        self.session = session
        self.no_losses = no_losses
        self.output_dir = output_dir
        self.self_test = self_test

    async def call(self, tool: str, args: dict[str, Any]) -> tuple[bool, Any]:
        resp = await self.session.call_tool(tool, args)
        if resp.is_error:
            msg = " ".join(item.text for item in resp.content if getattr(item, "text", None))
            return True, msg or "error"
        if resp.structured_content is not None:
            return False, resp.structured_content
        if resp.content and getattr(resp.content[0], "text", None):
            try:
                return False, json.loads(resp.content[0].text)
            except Exception:
                return False, resp.content[0].text
        return False, {}

    async def must(self, tool: str, args: dict[str, Any]) -> Any:
        err, value = await self.call(tool, args)
        if err:
            raise RuntimeError(f"{tool} failed: {value}")
        return value

    def check_phase(self, result: dict[str, Any], fixture: str, phase: str, diffs: list) -> None:
        """Accept diffs only if they exactly match the recorded known loss for this phase."""
        known = None if self.no_losses else KNOWN_LOSSES.get(fixture, {}).get(phase)
        if not diffs:
            return
        digest = hashlib.sha256(repr(diffs).encode("utf-8")).hexdigest()[:16]
        if known and known[0] == digest:
            result["losses"].append((phase, f"{len(diffs)} differences, digest {digest}: {known[1]}"))
            return
        key = "musicxml_roundtrip" if phase == "musicxml" else phase
        result[key], result["status"] = "failed", "failed"
        result["failures"].append(
            (
                phase,
                f"{len(diffs)} differences, digest {digest}" + (f" (recorded {known[0]})" if known else ""),
            )
        )
        result["failures"].extend((f"{phase}:{p}", f"{va!r} vs {vb!r}") for p, va, vb in diffs[:40])

    async def execute_case(
        self, case_id: str, fixture_ref: str, doc_id: str, excerpt_id: str | None = None
    ) -> dict[str, Any]:
        out = self.output_dir
        prefix = case_id
        result: dict[str, Any] = {
            "case": case_id,
            "fixture": fixture_ref,
            "excerptId": excerpt_id,
            "status": "passed",
            "mscz_roundtrip": "passed",
            "mscz_text": "passed",
            "musicxml_roundtrip": "passed",
            "idempotence": "passed",
            "losses": [],
            "failures": [],
        }

        # 1. Export MSCX of D (A)
        path_A = out / f"{prefix}_A.mscx"
        exp_args: dict[str, Any] = {
            "document": doc_id,
            "kind": "score",
            "options": {"format": "mscx"},
            "destination": {"kind": "file", "path": str(path_A)},
        }
        if excerpt_id is not None:
            exp_args["options"]["score"] = {"excerpt": excerpt_id}
        err, _ = await self.call("score_export", exp_args)
        if err:
            result["status"] = "failed"
            result["failures"].append(("export_mscx_A", "Failed to export initial MSCX"))
            return result
        sem_A = parse_mscx(path_A.read_text("utf-8"))

        # 2. MSCZ round trip -> B
        path_mscz = out / f"{prefix}.mscz"
        exp_args["options"]["format"], exp_args["destination"]["path"] = (
            "mscz",
            str(path_mscz),
        )
        await self.must("score_export", exp_args)

        err, opened_B = await self.call("score_open", {"path": str(path_mscz)})
        if err:
            result["status"] = "failed"
            result["failures"].append(("open_mscz", f"Failed to reopen MSCZ: {opened_B}"))
            return result
        doc_B = opened_B["document"]
        path_B = out / f"{prefix}_B.mscx"
        try:
            await self.must(
                "score_export",
                {
                    "document": doc_B,
                    "kind": "score",
                    "options": {"format": "mscx"},
                    "destination": {"kind": "file", "path": str(path_B)},
                },
            )

            # Check excerpts preservation on reopened MSCZ of master scores
            if excerpt_id is None:
                err_orig, exc_orig = await self.call("score_outline", {"document": doc_id})
                if err_orig or not isinstance(exc_orig, dict):
                    result["mscz_roundtrip"], result["status"] = "failed", "failed"
                    result["failures"].append(("mscz_excerpts", f"score_outline on source failed: {exc_orig}"))
                orig_excs = (
                    exc_orig.get("outline", {}).get("excerpts", [])
                    if not err_orig and isinstance(exc_orig, dict)
                    else []
                )
                if orig_excs:
                    err_b_exc, exc_b = await self.call("score_outline", {"document": doc_B})
                    if err_b_exc or not isinstance(exc_b, dict):
                        result["mscz_roundtrip"], result["status"] = "failed", "failed"
                        result["failures"].append(("mscz_excerpts", f"score_outline on doc_B failed: {exc_b}"))
                    else:
                        b_excs = exc_b.get("outline", {}).get("excerpts", [])
                        if len(orig_excs) != len(b_excs):
                            result["mscz_roundtrip"], result["status"] = (
                                "failed",
                                "failed",
                            )
                            result["failures"].append(
                                (
                                    "mscz_excerpts:count",
                                    f"{len(orig_excs)} vs {len(b_excs)}",
                                )
                            )
                        elif [e.get("name") for e in orig_excs] != [e.get("name") for e in b_excs]:
                            result["mscz_roundtrip"], result["status"] = (
                                "failed",
                                "failed",
                            )
                            result["failures"].append(
                                (
                                    "mscz_excerpts:titles",
                                    f"{[e.get('name') for e in orig_excs]} vs {[e.get('name') for e in b_excs]}",
                                )
                            )
                        else:
                            for idx, (e_a, e_b) in enumerate(zip(orig_excs, b_excs)):
                                p_exc_a = out / f"{prefix}_exc_{idx}_A.mscx"
                                p_exc_b = out / f"{prefix}_exc_{idx}_B.mscx"
                                for document, excerpt, path in (
                                    (doc_id, e_a["id"], p_exc_a),
                                    (doc_B, e_b["id"], p_exc_b),
                                ):
                                    await self.must(
                                        "score_export",
                                        {
                                            "document": document,
                                            "kind": "score",
                                            "options": {
                                                "format": "mscx",
                                                "score": {"excerpt": excerpt},
                                            },
                                            "destination": {
                                                "kind": "file",
                                                "path": str(path),
                                            },
                                        },
                                    )
                                sem_exc_a = parse_mscx(p_exc_a.read_text("utf-8"))
                                sem_exc_b = parse_mscx(p_exc_b.read_text("utf-8"))
                                exc_diffs = diff_data(sem_exc_a, sem_exc_b)
                                if exc_diffs:
                                    result["mscz_roundtrip"], result["status"] = (
                                        "failed",
                                        "failed",
                                    )
                                    result["failures"].extend(
                                        [
                                            (
                                                "mscz_excerpts:" + p,
                                                f"A={va!r} vs B={vb!r}",
                                            )
                                            for p, va, vb in exc_diffs[:40]
                                        ]
                                    )
        finally:
            await self.call("score_close", {"document": doc_B})

        sem_B = parse_mscx(path_B.read_text("utf-8"))
        if self.self_test and case_id == "create_edit" and sem_B["events"].get("1", {}).get(0):
            sem_B["events"]["1"][0] = sem_B["events"]["1"][0][:-1]  # deliberate mutation hook

        diffs_mscz = diff_data(sem_A, sem_B)
        if diffs_mscz:
            result["mscz_roundtrip"], result["status"] = "failed", "failed"
            result["failures"].extend([("mscz_rt:" + p, f"A={va!r} vs B={vb!r}") for p, va, vb in diffs_mscz[:40]])

        # Full XML comparison still catches properties outside the music parser;
        # only the exact documented page-style default insertion is normalized.
        xml_diffs = compare_mscz_xml(path_A.read_text("utf-8"), path_B.read_text("utf-8"))
        self.check_phase(result, fixture_ref, "mscz_text", xml_diffs)

        # 3. MusicXML round trip -> C
        path_xml1 = out / f"{prefix}_1.musicxml"
        exp_args["options"]["format"], exp_args["destination"]["path"] = (
            "musicxml",
            str(path_xml1),
        )
        await self.must("score_export", exp_args)

        err, opened_C = await self.call("score_open", {"path": str(path_xml1)})
        if err:
            result["musicxml_roundtrip"], result["status"] = "failed", "failed"
            result["failures"].append(("open_musicxml", str(opened_C)))
            return result
        doc_C = opened_C["document"]
        path_C = out / f"{prefix}_C.mscx"
        path_xml2 = out / f"{prefix}_2.musicxml"
        try:
            for fmt, path in (("mscx", path_C), ("musicxml", path_xml2)):
                await self.must(
                    "score_export",
                    {
                        "document": doc_C,
                        "kind": "score",
                        "options": {"format": fmt},
                        "destination": {"kind": "file", "path": str(path)},
                    },
                )
        finally:
            await self.call("score_close", {"document": doc_C})

        sem_C = parse_mscx(path_C.read_text("utf-8"))
        self.check_phase(
            result,
            fixture_ref,
            "musicxml",
            diff_data(to_musicxml_subset(sem_A), to_musicxml_subset(sem_C)),
        )

        # 4. Idempotence: a second MusicXML generation must be a semantic fixpoint.
        # Text comparison is useless here (layout coordinates are re-derived).
        err, opened_D = await self.call("score_open", {"path": str(path_xml2)})
        if err:
            result["idempotence"], result["status"] = "failed", "failed"
            result["failures"].append(("idempotence", f"Failed to reopen second MusicXML: {opened_D}"))
            return result
        path_D = out / f"{prefix}_D.mscx"
        try:
            await self.must(
                "score_export",
                {
                    "document": opened_D["document"],
                    "kind": "score",
                    "options": {"format": "mscx"},
                    "destination": {"kind": "file", "path": str(path_D)},
                },
            )
        finally:
            await self.call("score_close", {"document": opened_D["document"]})
        self.check_phase(
            result,
            fixture_ref,
            "idempotence",
            diff_data(sem_C, parse_mscx(path_D.read_text("utf-8"))),
        )

        return result


async def run_suite(
    binary: Path,
    resources: Path,
    output_dir: Path,
    self_test: bool = False,
    no_losses: bool = False,
) -> int:
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    native_lib = pixi_env(REPO_ROOT) / "Library/bin"
    if os.name == "nt" and native_lib.exists():
        env["PATH"] = str(native_lib) + os.pathsep + env.get("PATH", "")

    params = StdioServerParameters(
        command=str(binary.resolve()),
        args=["--resources", str(resources.resolve())],
        env=env,
        cwd=str(output_dir.resolve()),
    )
    runner_report: dict[str, Any] = {
        "binary": str(binary.resolve()),
        "resources": str(resources.resolve()),
        "selfTest": self_test,
        "cases": [],
        "stale_known_losses": [],
        "passed": False,
    }

    t0 = time.perf_counter()
    with (output_dir / "stderr.log").open("w", encoding="utf-8") as stderr_file:
        async with stdio_client(params, errlog=stderr_file) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                runner = RoundTripRunner(session, output_dir, self_test, no_losses)

                # Case 1: create+edit
                err, res = await runner.call(
                    "score_create",
                    {"template_id": "Treble_Clef", "title": "Round trip"},
                )
                if err:
                    runner_report["cases"].append(
                        {
                            "case": "create_edit",
                            "fixture": "template:Treble_Clef",
                            "status": "failed",
                            "losses": [],
                            "failures": [("score_create", str(res))],
                        }
                    )
                else:
                    doc_create = res["document"]
                    try:
                        await runner.must(
                            "score_edit",
                            {
                                "document": doc_create,
                                "ops": [{"op": "insert_measures", "count": 1}],
                            },
                        )
                        outline = await runner.must("score_outline", {"document": doc_create})
                        measures = outline["outline"]["measures"]
                        assert len(measures) >= 2
                        for measure, pitches in zip(measures[:2], ((60, 62, 64, 65), (76, 74, 72, 71))):
                            for beat, value in enumerate(pitches):
                                tick = Fraction(
                                    int(measure["tick"]["num"]),
                                    int(measure["tick"]["den"]),
                                ) + Fraction(beat, 4)
                                await runner.must(
                                    "score_edit",
                                    {
                                        "document": doc_create,
                                        "ops": [
                                            {
                                                "op": "enter_notes",
                                                "at": {
                                                    "kind": "segment",
                                                    "measure": measure["id"],
                                                    "staff": 0,
                                                    "voice": 0,
                                                    "tick": {
                                                        "num": str(tick.numerator),
                                                        "den": str(tick.denominator),
                                                    },
                                                    "segment_kind": "ChordRest",
                                                },
                                                "duration": {"num": "1", "den": "4"},
                                                "pitches": [{"pitch": value}],
                                            }
                                        ],
                                    },
                                )
                        q = await runner.must(
                            "score_query",
                            {
                                "document": doc_create,
                                "mode": "elements",
                                "query": {
                                    "types": ["Note"],
                                    "first_measure": 0,
                                    "measure_count": 1,
                                },
                            },
                        )
                        await runner.must(
                            "score_edit",
                            {
                                "document": doc_create,
                                "expected_revision": q["revision"],
                                "ops": [
                                    {
                                        "op": "set_pitch",
                                        "note": {
                                            "kind": "id",
                                            "id": q["elements"][0]["id"],
                                        },
                                        "pitch": 68,
                                    }
                                ],
                            },
                        )
                        rep1 = await runner.execute_case("create_edit", "template:Treble_Clef", doc_create)
                        runner_report["cases"].append(rep1)
                    finally:
                        await runner.call("score_close", {"document": doc_create})

                # Case 2: import all golden fixtures except created.json / README.md, + evening.musicxml
                golden_dir = REPO_ROOT / "vendor/engrave/tests/data/inputs/golden"
                import_files = sorted(
                    [p for p in golden_dir.glob("*") if p.is_file() and p.name not in ("created.json", "README.md")]
                    + [REPO_ROOT / "vendor/engrave/tests/data/inputs/evening.musicxml"]
                )
                for path in import_files:
                    cid = f"import_{path.stem}"
                    err, opened = await runner.call("score_open", {"path": str(path.resolve())})
                    if err:
                        runner_report["cases"].append(
                            {
                                "case": cid,
                                "fixture": path.name,
                                "status": "failed",
                                "losses": [],
                                "failures": [("score_open", str(opened))],
                            }
                        )
                        continue

                    doc = opened["document"]
                    try:
                        rep = await runner.execute_case(cid, path.name, doc)
                        runner_report["cases"].append(rep)
                        if path.name == "linked_parts.mscx":
                            # Case 3: excerpts for linked_parts.mscx
                            err_exc, exc_data = await runner.call("score_outline", {"document": doc})
                            if err_exc or not isinstance(exc_data, dict):
                                runner_report["cases"].append(
                                    {
                                        "case": f"excerpts_{path.stem}",
                                        "fixture": path.name,
                                        "status": "failed",
                                        "losses": [],
                                        "failures": [("score_outline", str(exc_data))],
                                    }
                                )
                            else:
                                for index, exc in enumerate(exc_data["outline"]["excerpts"]):
                                    eid = exc["id"]
                                    rep_exc = await runner.execute_case(
                                        f"excerpt_{path.stem}_{index}",
                                        f"{path.name}:excerpt:{index}",
                                        doc,
                                        excerpt_id=eid,
                                    )
                                    runner_report["cases"].append(rep_exc)
                    finally:
                        await runner.call("score_close", {"document": doc})

    elapsed = round(time.perf_counter() - t0, 3)
    runner_report["elapsed_seconds"] = elapsed

    # An entry is stale when its phase produced no accepted difference (or never ran).
    stale = (
        []
        if no_losses
        else sorted(
            (f, phase)
            for f, phases in KNOWN_LOSSES.items()
            for phase in phases
            if not any(
                c.get("fixture") == f and any(loss[0] == phase for loss in c.get("losses", []))
                for c in runner_report["cases"]
            )
        )
    )
    runner_report["stale_known_losses"] = [{"fixture": f, "phase": phase} for f, phase in stale]
    for f, phase in stale:
        print(f"WARNING: stale KNOWN_LOSSES entry [{f}] {phase}")

    all_passed = all(c["status"] == "passed" for c in runner_report["cases"])
    runner_report["passed"] = all_passed
    (output_dir / "report.json").write_text(json.dumps(runner_report, indent=2), encoding="utf-8")

    total = len(runner_report["cases"])
    passed_count = sum(1 for c in runner_report["cases"] if c["status"] == "passed")
    print(f"Round-trip suite finished in {elapsed}s: {passed_count}/{total} cases passed.")
    for c in runner_report["cases"]:
        tag = "[PASS]" if c["status"] == "passed" else "[FAIL]"
        print(f"  {tag} {c['case']} ({c.get('fixture', '')})")
        for f, detail in c.get("failures", []):
            print(f"      Failure in {f}: {detail}")

    return 0 if all_passed else 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="Path to engrave-mcp server binary")
    parser.add_argument(
        "--resources",
        type=Path,
        required=True,
        help="Path to .runtime/assets directory",
    )
    parser.add_argument("--output", type=Path, required=True, help="Scratch output directory")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Deliberately mutate B to verify parser catches failures",
    )
    parser.add_argument(
        "--no-losses",
        action="store_true",
        help="Ignore KNOWN_LOSSES and report every difference",
    )
    args = parser.parse_args()

    exit_code = asyncio.run(
        run_suite(
            args.binary,
            args.resources,
            args.output,
            self_test=args.self_test,
            no_losses=args.no_losses,
        )
    )
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
