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
from pathlib import Path
import sys
import time
from typing import Any
import xml.etree.ElementTree as ET
from fractions import Fraction

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
# Known MusicXML interop losses observed at the pre-fork baseline (f8e7d26).
# {fixture: {phase: (digest of the exact difference list, justification)}}; phase is
# "musicxml" (A vs C subset) or "idempotence" (C vs D). A different difference list
# fails with its new digest; an entry whose phase no longer differs is reported stale.
GRACE_AFTER = "grace-after notes come back as grace-before notes of the next chord (MusicXML has no grace-after)"
KNOWN_LOSSES: dict[str, dict[str, tuple[str, str]]] = {
    "bend.gp": {"musicxml": ("a6f7d6dea4668595", "GP bend grace notes: " + GRACE_AFTER)},
    "hairpins_and_repeats.mscx": {"musicxml": ("05dd75d215fedcc1", "invisible fff dynamic is not exported")},
    "linked_parts.mscx": {
        "musicxml": ("7aad2a9774db7ac7", "grace8after with ties: " + GRACE_AFTER),
        "idempotence": ("810036ddfbce08d1", "the tie re-attached to the moved grace note is dropped on the second import"),
    },
    "linked_parts.mscx:excerpt:0": {
        "musicxml": ("7aad2a9774db7ac7", "grace8after with ties: " + GRACE_AFTER),
        "idempotence": ("810036ddfbce08d1", "the tie re-attached to the moved grace note is dropped on the second import"),
    },
    "slurs.mscz": {"musicxml": ("b95cd012de8466e0", "grace16after slurs: " + GRACE_AFTER)},
}

DUR_TYPES: dict[str, Fraction | int] = {
    "longa": 16, "breve": 8, "whole": 4, "half": 2, "quarter": 1,
    "eighth": Fraction(1, 2), "16th": Fraction(1, 4), "32nd": Fraction(1, 8),
    "64th": Fraction(1, 16), "128th": Fraction(1, 32), "256th": Fraction(1, 64),
    "512th": Fraction(1, 128), "1024th": Fraction(1, 256),
}
ALLOWED_SPANNERS = {"Slur", "HairPin", "Volta", "Pedal", "Ottava", "Tie"}


def frac_to_ticks(frac_str: str | None, div: int) -> int:
    """Convert an MSCX fraction string ('N/D' or 'N') into ticks."""
    if not frac_str:
        return 0
    return int(Fraction(frac_str.strip()) * 4 * div)


def parse_mscx(text: str) -> dict[str, Any]:
    """Parse a MuseScore 4 MSCX document into its semantic representation."""
    root = ET.fromstring(text)
    score = root.find("Score")
    if score is None:
        score = root
    div = int(score.findtext("Division", "480"))

    # Metadata: metaTag name->value, excluding creationDate and platform.
    metadata: dict[str, str] = {}
    for m in score.findall("metaTag"):
        name = m.get("name")
        if name and name not in ("creationDate", "platform"):
            metadata[name] = (m.text or "").strip()

    # Parts: trackName / Instrument instrumentId / longName, staff count, StaffType group.
    parts: list[dict[str, Any]] = []
    for p in score.findall("Part"):
        inst = p.find("Instrument")
        staves_p = p.findall("Staff")
        groups = [s.find("StaffType").get("group", "") for s in staves_p if s.find("StaffType") is not None]
        parts.append({
            "trackName": p.findtext("trackName", "").strip(),
            "instrumentId": inst.findtext("instrumentId", "").strip() if inst is not None else "",
            "longName": inst.findtext("longName", "").strip() if inst is not None else "",
            "staffCount": len(staves_p),
            "staffTypeGroups": groups,
        })

    # Measures per staff: count, timesig/len, start/end repeat, key sig changes.
    staves = score.findall("Staff")
    meas_ticks: list[tuple[int, int, tuple[int, int]]] = []
    curr_tick, curr_sig = 0, (4, 4)
    if staves:
        for m in staves[0].findall("Measure"):
            ts = m.find(".//TimeSig")
            if ts is not None:
                curr_sig = (int(ts.findtext("sigN", "4")), int(ts.findtext("sigD", "4")))
            mlen = frac_to_ticks(m.findtext("len"), div) if m.findtext("len") else int(Fraction(curr_sig[0], curr_sig[1]) * 4 * div)
            meas_ticks.append((curr_tick, mlen, curr_sig))
            curr_tick += mlen

    staff_measures, staff_events, staff_spanners, staff_annotations = {}, {}, {}, {}
    for s_idx, staff in enumerate(staves):
        staff_id = staff.get("id", str(s_idx + 1))
        measures_info, events_by_voice, spanners_list = [], {}, []
        dyn_counts, tempo_list, lyrics_list, artic_counts, fingering_list, staff_text_list = {}, [], [], {}, [], []
        harmony_count, fermata_count = 0, 0

        m_list = staff.findall("Measure")
        for m_idx, m in enumerate(m_list):
            m_start, mlen, _ = meas_ticks[m_idx] if m_idx < len(meas_ticks) else (0, 1920, (4, 4))
            ts = m.find(".//TimeSig")
            ts_str = f"{ts.findtext('sigN')}/{ts.findtext('sigD')}" if ts is not None else None
            ks = m.find(".//KeySig")
            key_val = (ks.findtext("concertKey") or ks.findtext("accidental")) if ks is not None else None
            measures_info.append({
                "timesig": ts_str,
                "len": m.findtext("len"),
                "startRepeat": bool(m.find("startRepeat") is not None),
                "endRepeat": int(m.findtext("endRepeat")) if m.find("endRepeat") is not None else 0,
                "keySig": key_val,
            })

            # Events per voice: chords and rests with absolute tick & duration ticks
            for v_idx, v in enumerate(m.findall("voice")):
                if v_idx not in events_by_voice:
                    events_by_voice[v_idx] = []
                v_tick = m_start
                tuplet_stack: list[tuple[int, int]] = []
                for child in v:
                    if child.tag == "Tuplet":
                        tuplet_stack.append((int(child.findtext("normalNotes", "2")), int(child.findtext("actualNotes", "3"))))
                    elif child.tag == "endTuplet" and tuplet_stack:
                        tuplet_stack.pop()
                    elif child.tag == "location":
                        if child.findtext("fractions"):
                            v_tick += frac_to_ticks(child.findtext("fractions"), div)
                    elif child.tag == "Chord":
                        is_grace = any(c.tag.startswith(("grace", "appoggiatura", "acciaccatura")) for c in child)
                        if is_grace:
                            dur = 0
                        elif child.findtext("duration"):
                            dur = frac_to_ticks(child.findtext("duration"), div)
                        else:
                            dtype = child.findtext("durationType", "quarter")
                            dots = int(child.findtext("dots", "0"))
                            base = int(DUR_TYPES.get(dtype, 1) * div)
                            dur = base + sum(base >> (d + 1) for d in range(dots))
                            for norm, act in tuplet_stack:
                                dur = (dur * norm) // act
                        notes = []
                        for n in child.findall("Note"):
                            pitch, tpc = int(n.findtext("pitch", "0")), int(n.findtext("tpc", "0"))
                            tie_start = any(sp.get("type") == "Tie" and sp.find("next") is not None for sp in n.findall("Spanner")) or (n.find("Tie") is not None and n.find("Tie/prev") is None)
                            notes.append((pitch, tpc, tie_start))
                            for f in n.findall("Fingering"):
                                fingering_list.append(f.findtext("text", "").strip())
                            for sp in n.findall("Spanner"):
                                if sp.get("type") == "Tie" and sp.find("next/location") is not None:
                                    f_off = frac_to_ticks(sp.find("next/location").findtext("fractions", "0"), div)
                                    spanners_list.append(("Tie", staff_id, v_tick, v_tick + f_off))
                        events_by_voice[v_idx].append(("chord", v_tick, dur, tuple(sorted(notes))))
                        for art in child.findall("Articulation"):
                            sub = art.findtext("subtype") or art.get("name") or ""
                            artic_counts[sub] = artic_counts.get(sub, 0) + 1
                        for lyr in child.findall("Lyrics"):
                            lyrics_list.append((int(lyr.findtext("no", "0")), lyr.findtext("text", "").strip()))
                        fermata_count += len(child.findall("Fermata"))
                        for sp in child.findall("Spanner"):
                            nxt = sp.find("next/location")
                            if nxt is not None:
                                m_off = int(nxt.findtext("measures", "0"))
                                f_off = frac_to_ticks(nxt.findtext("fractions", "0"), div)
                                target_m = m_idx + m_off
                                target_start = meas_ticks[target_m][0] if target_m < len(meas_ticks) else v_tick
                                spanners_list.append((sp.get("type", ""), staff_id, v_tick, target_start + (v_tick - m_start) + f_off))
                        v_tick += dur
                    elif child.tag == "Rest":
                        if child.findtext("duration"):
                            dur = frac_to_ticks(child.findtext("duration"), div)
                        elif child.findtext("durationType") == "measure":
                            dur = mlen
                        else:
                            dtype = child.findtext("durationType", "quarter")
                            dots = int(child.findtext("dots", "0"))
                            base = int(DUR_TYPES.get(dtype, 1) * div)
                            dur = base + sum(base >> (d + 1) for d in range(dots))
                            for norm, act in tuplet_stack:
                                dur = (dur * norm) // act
                        events_by_voice[v_idx].append(("rest", v_tick, dur, ()))
                        fermata_count += len(child.findall("Fermata"))
                        v_tick += dur
                    elif child.tag == "Dynamic":
                        sub = child.findtext("subtype", "").strip()
                        dyn_counts[sub] = dyn_counts.get(sub, 0) + 1
                    elif child.tag == "Tempo":
                        tempo_list.append(round(float(child.findtext("tempo", "0")), 4))
                    elif child.tag == "Harmony":
                        harmony_count += 1
                    elif child.tag == "StaffText":
                        staff_text_list.append(child.findtext("text", "").strip())
                    elif child.tag == "Spanner":
                        nxt = child.find("next/location")
                        if nxt is not None:
                            m_off = int(nxt.findtext("measures", "0"))
                            f_off = frac_to_ticks(nxt.findtext("fractions", "0"), div)
                            target_m = m_idx + m_off
                            target_start = meas_ticks[target_m][0] if target_m < len(meas_ticks) else v_tick
                            spanners_list.append((child.get("type", ""), staff_id, v_tick, target_start + (v_tick - m_start) + f_off))

        staff_measures[staff_id] = measures_info
        staff_events[staff_id] = events_by_voice
        staff_spanners[staff_id] = sorted(spanners_list)
        staff_annotations[staff_id] = {
            "dynamics": dyn_counts, "tempos": tempo_list, "harmony": harmony_count,
            "lyrics": lyrics_list, "articulations": artic_counts, "fingerings": fingering_list,
            "staffTexts": staff_text_list, "fermatas": fermata_count,
        }

    excerpts = [{"title": s.findtext("name") or "", "parts": [p.get("id") for p in s.findall("Part")]} for s in score.findall("Score")]
    return {
        "metadata": metadata, "parts": parts, "measures": staff_measures,
        "events": staff_events, "spanners": staff_spanners,
        "annotations": staff_annotations, "excerpts": excerpts,
    }


def to_musicxml_subset(sem: dict[str, Any]) -> dict[str, Any]:
    """Project semantic representation down to MusicXML-comparable subset."""
    meta = {
        "title": sem["metadata"].get("workTitle", "") or sem["metadata"].get("movementTitle", ""),
        "composer": sem["metadata"].get("composer", ""),
    }
    parts = [{"name": p["longName"] or p["trackName"], "staffCount": p["staffCount"]} for p in sem["parts"]]
    measures = {
        sid: {"count": len(mlist), "measures": [{"timesig": m["timesig"], "startRepeat": m["startRepeat"], "endRepeat": m["endRepeat"]} for m in mlist]}
        for sid, mlist in sem["measures"].items()
    }
    events = {
        sid: {
            vid: [(kind, tick, dur, tuple((p, tie) for p, tpc, tie in notes)) for kind, tick, dur, notes in elist]
            for vid, elist in vmap.items()
        }
        for sid, vmap in sem["events"].items()
    }
    spanners = {sid: [sp for sp in splist if sp[0] in ALLOWED_SPANNERS] for sid, splist in sem["spanners"].items()}
    annotations = {
        sid: {"dynamics": ann["dynamics"], "tempos": ann["tempos"], "lyrics": ann["lyrics"], "harmony": ann["harmony"]}
        for sid, ann in sem["annotations"].items()
    }
    return {
        "metadata": meta, "parts": parts, "measures": measures,
        "events": events, "spanners": spanners, "annotations": annotations,
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
    def __init__(self, session: ClientSession, output_dir: Path, self_test: bool = False, no_losses: bool = False):
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
        result["failures"].append((phase, f"{len(diffs)} differences, digest {digest}"
                                   + (f" (recorded {known[0]})" if known else "")))
        result["failures"].extend((f"{phase}:{p}", f"{va!r} vs {vb!r}") for p, va, vb in diffs[:40])

    async def execute_case(self, case_id: str, fixture_ref: str, doc_id: str, excerpt_id: int = -1) -> dict[str, Any]:
        out = self.output_dir
        prefix = f"{case_id}_{excerpt_id}" if excerpt_id >= 0 else case_id
        result: dict[str, Any] = {
            "case": case_id, "fixture": fixture_ref, "excerptId": excerpt_id,
            "status": "passed", "mscz_roundtrip": "passed",
            "musicxml_roundtrip": "passed", "idempotence": "passed",
            "losses": [], "failures": [],
        }

        # 1. Export MSCX of D (A)
        path_A = out / f"{prefix}_A.mscx"
        exp_args: dict[str, Any] = {"documentId": doc_id, "format": "mscx", "path": str(path_A)}
        if excerpt_id >= 0:
            exp_args["excerptId"] = excerpt_id
        err, _ = await self.call("score_export", exp_args)
        if err:
            result["status"] = "failed"
            result["failures"].append(("export_mscx_A", "Failed to export initial MSCX"))
            return result
        sem_A = parse_mscx(path_A.read_text("utf-8"))

        # 2. MSCZ round trip -> B
        path_mscz = out / f"{prefix}.mscz"
        exp_args["format"], exp_args["path"] = "mscz", str(path_mscz)
        await self.must("score_export", exp_args)

        err, opened_B = await self.call("score_open", {"path": str(path_mscz)})
        if err:
            result["status"] = "failed"
            result["failures"].append(("open_mscz", f"Failed to reopen MSCZ: {opened_B}"))
            return result
        doc_B = opened_B["documentId"]
        path_B = out / f"{prefix}_B.mscx"
        try:
            await self.must("score_export", {"documentId": doc_B, "format": "mscx", "path": str(path_B)})
        finally:
            await self.call("score_close", {"documentId": doc_B})

        sem_B = parse_mscx(path_B.read_text("utf-8"))
        if self.self_test and case_id == "create_edit" and sem_B["events"].get("1", {}).get(0):
            sem_B["events"]["1"][0] = sem_B["events"]["1"][0][:-1]  # deliberate mutation hook

        diffs_mscz = diff_data(sem_A, sem_B)
        if diffs_mscz:
            result["mscz_roundtrip"], result["status"] = "failed", "failed"
            result["failures"].extend([("mscz_rt:" + p, f"A={va!r} vs B={vb!r}") for p, va, vb in diffs_mscz[:40]])

        # 3. MusicXML round trip -> C
        path_xml1 = out / f"{prefix}_1.musicxml"
        exp_args["format"], exp_args["path"] = "musicxml", str(path_xml1)
        await self.must("score_export", exp_args)

        err, opened_C = await self.call("score_open", {"path": str(path_xml1)})
        if err:
            result["musicxml_roundtrip"], result["status"] = "failed", "failed"
            result["failures"].append(("open_musicxml", str(opened_C)))
            return result
        doc_C = opened_C["documentId"]
        path_C = out / f"{prefix}_C.mscx"
        path_xml2 = out / f"{prefix}_2.musicxml"
        try:
            await self.must("score_export", {"documentId": doc_C, "format": "mscx", "path": str(path_C)})
            await self.must("score_export", {"documentId": doc_C, "format": "musicxml", "path": str(path_xml2)})
        finally:
            await self.call("score_close", {"documentId": doc_C})

        sem_C = parse_mscx(path_C.read_text("utf-8"))
        self.check_phase(result, fixture_ref, "musicxml",
                         diff_data(to_musicxml_subset(sem_A), to_musicxml_subset(sem_C)))

        # 4. Idempotence: a second MusicXML generation must be a semantic fixpoint.
        # Text comparison is useless here (layout coordinates are re-derived).
        err, opened_D = await self.call("score_open", {"path": str(path_xml2)})
        if err:
            result["idempotence"], result["status"] = "failed", "failed"
            result["failures"].append(("idempotence", f"Failed to reopen second MusicXML: {opened_D}"))
            return result
        path_D = out / f"{prefix}_D.mscx"
        try:
            await self.must("score_export", {"documentId": opened_D["documentId"], "format": "mscx", "path": str(path_D)})
        finally:
            await self.call("score_close", {"documentId": opened_D["documentId"]})
        self.check_phase(result, fixture_ref, "idempotence",
                         diff_data(sem_C, parse_mscx(path_D.read_text("utf-8"))))

        return result


async def run_suite(binary: Path, resources: Path, output_dir: Path, self_test: bool = False, no_losses: bool = False) -> int:
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    native_lib = ROOT / ".pixi/envs/native/Library/bin"
    if os.name == "nt" and native_lib.exists():
        env["PATH"] = str(native_lib) + os.pathsep + env.get("PATH", "")

    params = StdioServerParameters(
        command=str(binary.resolve()),
        args=["--resources", str(resources.resolve())],
        env=env,
        cwd=str(output_dir.resolve()),
    )
    runner_report: dict[str, Any] = {
        "binary": str(binary.resolve()), "resources": str(resources.resolve()),
        "selfTest": self_test, "cases": [], "stale_known_losses": [], "passed": False,
    }

    t0 = time.perf_counter()
    with (output_dir / "stderr.log").open("w", encoding="utf-8") as stderr_file:
        async with stdio_client(params, errlog=stderr_file) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                runner = RoundTripRunner(session, output_dir, self_test, no_losses)

                # Case 1: create+edit
                created_fixture = ROOT / "fixtures/golden/created.json"
                score_data = json.loads(created_fixture.read_text("utf-8"))
                err, res = await runner.call("score_create", {"score": score_data})
                assert not err, f"score_create failed: {res}"
                doc_create = res["documentId"]
                try:
                    rep_meas = {"notes": [{"pitches": [p], "ticks": 480} for p in (76, 74, 72, 71)]}
                    await runner.must("score_replace_measures", {
                        "documentId": doc_create, "partIndex": 0, "startMeasure": 1, "measures": [rep_meas],
                    })
                    q = await runner.must("score_query_region", {
                        "documentId": doc_create, "partIndex": 0, "firstMeasure": 0, "measureCount": 1,
                    })
                    await runner.must("score_set_pitches", {
                        "documentId": doc_create, "baseRevision": q["revision"], "operationId": "roundtrip-edit-1",
                        "changes": [{"noteId": q["notes"][0]["id"], "pitch": 68}],
                    })
                    rep1 = await runner.execute_case("create_edit", "created.json", doc_create)
                    runner_report["cases"].append(rep1)
                finally:
                    await runner.call("score_close", {"documentId": doc_create})

                # Case 2: import all golden fixtures except created.json / README.md, + evening.musicxml
                golden_dir = ROOT / "fixtures/golden"
                import_files = sorted(
                    [p for p in golden_dir.glob("*") if p.is_file() and p.name not in ("created.json", "README.md")]
                    + [ROOT / "fixtures/evening.musicxml"]
                )
                for path in import_files:
                    cid = f"import_{path.stem}"
                    err, opened = await runner.call("score_open", {"path": str(path.resolve())})
                    if err:
                        runner_report["cases"].append({"case": cid, "fixture": path.name, "status": "failed",
                                                       "losses": [], "failures": [("score_open", str(opened))]})
                        continue

                    doc = opened["documentId"]
                    try:
                        rep = await runner.execute_case(cid, path.name, doc)
                        runner_report["cases"].append(rep)
                        if path.name == "linked_parts.mscx":
                            # Case 3: excerpts for linked_parts.mscx
                            _, exc_data = await runner.call("score_list_excerpts", {"documentId": doc})
                            for exc in exc_data.get("excerpts", []):
                                eid = exc["id"]
                                rep_exc = await runner.execute_case(
                                    f"excerpt_{path.stem}_{eid}", f"{path.name}:excerpt:{eid}", doc, excerpt_id=eid,
                                )
                                runner_report["cases"].append(rep_exc)
                    finally:
                        await runner.call("score_close", {"documentId": doc})

    elapsed = round(time.perf_counter() - t0, 3)
    runner_report["elapsed_seconds"] = elapsed

    # An entry is stale when its phase produced no accepted difference (or never ran).
    stale = [] if no_losses else sorted(
        (f, phase) for f, phases in KNOWN_LOSSES.items() for phase in phases
        if not any(c.get("fixture") == f and any(loss[0] == phase for loss in c.get("losses", []))
                   for c in runner_report["cases"]))
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
    parser.add_argument("--binary", type=Path, required=True, help="Path to musescore-mcp server binary")
    parser.add_argument("--resources", type=Path, required=True, help="Path to .runtime/assets directory")
    parser.add_argument("--output", type=Path, required=True, help="Scratch output directory")
    parser.add_argument("--self-test", action="store_true", help="Deliberately mutate B to verify parser catches failures")
    parser.add_argument("--no-losses", action="store_true", help="Ignore KNOWN_LOSSES and report every difference")
    args = parser.parse_args()

    exit_code = asyncio.run(run_suite(args.binary, args.resources, args.output, self_test=args.self_test, no_losses=args.no_losses))
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
