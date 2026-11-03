"""Measure and record writer/round-trip precision baseline for phase B (DEFECT-ENGRAVE-016)."""
from __future__ import annotations

import argparse
import zipfile
import xml.etree.ElementTree as ET
import platform
import asyncio
import hashlib
import json
import re
import statistics
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image
import pymupdf
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
BINARY = RESOURCES = SOUNDFONT = PRECISION_DIR = None
VERSION = "4.70"

FIXTURES = [
    {"name": "repeat70.mscx", "type": "plain", "format": "mscx", "has_excerpt": False},
    {"name": "lyrics.mscx", "type": "lyrics/text", "format": "mscx", "has_excerpt": False},
    {"name": "hairpins_and_repeats.mscx", "type": "spanners/repeats", "format": "mscx", "has_excerpt": False},
    {"name": "drums_import.musicxml", "type": "imported MusicXML", "format": "musicxml", "has_excerpt": False},
    {"name": "linked_parts.mscx", "type": "excerpt-bearing", "format": "mscx", "has_excerpt": True, "excerpt_id": "v_v"},
    {"name": "coverage.musicxml", "type": "imported MusicXML (secondary)", "format": "musicxml", "has_excerpt": False},
]

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def describe_pdf(path: Path) -> dict:
    doc = pymupdf.open(stream=path.read_bytes(), filetype="pdf")
    try:
        pages = doc.page_count
        p0 = doc[0]
        rect = [p0.rect.x0, p0.rect.y0, p0.rect.x1, p0.rect.y1]
        width_pt = p0.rect.width
        height_pt = p0.rect.height
        doc.set_metadata({k: "" for k in ("title", "author", "subject", "keywords", "creator",
                                          "producer", "creationDate", "modDate")})
        canonical = doc.tobytes(garbage=3, deflate=True, no_new_id=True)
    finally:
        doc.close()
    canonical = re.sub(rb"/(CreationDate|ModDate|Producer)\s*\([^)]*\)", rb"/\1 ()", canonical)
    return {
        "pages": pages,
        "width_pt": width_pt,
        "height_pt": height_pt,
        "rect": rect,
        "canonical_bytes": len(canonical),
        "sha256": sha256_bytes(canonical)
    }

def compare_vectors_pdf(p_src: Path, p_reop: Path) -> dict:
    with pymupdf.open(stream=p_src.read_bytes(), filetype="pdf") as a, \
         pymupdf.open(stream=p_reop.read_bytes(), filetype="pdf") as b:
        p_a, p_b = a[0], b[0]
        rect_delta = [abs(p_a.rect.width - p_b.rect.width), abs(p_a.rect.height - p_b.rect.height)]
        text_a = p_a.get_text()
        text_b = p_b.get_text()
        text_equal = (text_a == text_b)
        
        words_a = p_a.get_text("words")
        words_b = p_b.get_text("words")
        words_count_a = len(words_a)
        words_count_b = len(words_b)
        
        drawings_a = p_a.get_drawings()
        drawings_b = p_b.get_drawings()
        drawings_count_a = len(drawings_a)
        drawings_count_b = len(drawings_b)

        max_coord_delta = 0.0
        # Compare drawing rects if counts match
        if drawings_count_a == drawings_count_b:
            for da, db in zip(drawings_a, drawings_b):
                ra = da.get("rect")
                rb = db.get("rect")
                if ra and rb:
                    for val_a, val_b in zip(tuple(ra), tuple(rb)):
                        max_coord_delta = max(max_coord_delta, abs(val_a - val_b))
        else:
            max_coord_delta = None

        return {
            "rect_delta": rect_delta,
            "text_equal": text_equal,
            "words_counts": [words_count_a, words_count_b],
            "drawings_counts": [drawings_count_a, drawings_count_b],
            "max_coord_delta_pt": max_coord_delta
        }

def compare_png(p_src: Path, p_reop: Path) -> dict:
    with Image.open(p_src) as img1, Image.open(p_reop) as img2:
        w1, h1 = img1.size
        w2, h2 = img2.size
        cw, ch = min(w1, w2), min(h1, h2)
        arr1 = np.asarray(img1.convert("RGBA"))
        arr2 = np.asarray(img2.convert("RGBA"))
        
        arr1_c = arr1[:ch, :cw]
        arr2_c = arr2[:ch, :cw]
        
        diff_mask = np.any(arr1_c != arr2_c, axis=2)
        pixel_diff = int(np.count_nonzero(diff_mask))
        
        size_delta = [w2 - w1, h2 - h1]
        pixel_limit = max(100, cw * ch // 20000)
        
        # Check if extra border is blank (transparent or background white)
        extra_border_blank = True
        if w1 != w2 or h1 != h2:
            # Check border of img2 or img1 depending on which is larger
            if w2 > w1:
                extra = arr2[:, cw:]
                # blank if alpha == 0 or rgb == 255
                if not np.all((extra[:, :, 3] == 0) | (np.all(extra[:, :, :3] == 255, axis=2))):
                    extra_border_blank = False
            if h2 > h1:
                extra = arr2[ch:, :]
                if not np.all((extra[:, :, 3] == 0) | (np.all(extra[:, :, :3] == 255, axis=2))):
                    extra_border_blank = False
            if w1 > w2:
                extra = arr1[:, cw:]
                if not np.all((extra[:, :, 3] == 0) | (np.all(extra[:, :, :3] == 255, axis=2))):
                    extra_border_blank = False
            if h1 > h2:
                extra = arr1[ch:, :]
                if not np.all((extra[:, :, 3] == 0) | (np.all(extra[:, :, :3] == 255, axis=2))):
                    extra_border_blank = False
        
        within_tolerance = (
            abs(size_delta[0]) <= 1 and abs(size_delta[1]) <= 1
            and pixel_diff <= pixel_limit
            and extra_border_blank
        )

        return {
            "src_size": [w1, h1],
            "reop_size": [w2, h2],
            "size_delta": size_delta,
            "compared_dimensions": [cw, ch],
            "pixel_diff": pixel_diff,
            "pixel_limit": pixel_limit,
            "extra_border_blank": extra_border_blank,
            "within_tolerance": within_tolerance,
            "src_sha256": sha256_bytes(arr1.tobytes()),
            "reop_sha256": sha256_bytes(arr2.tobytes())
        }

def compare_positions(src_pos: dict, reop_pos: dict) -> dict:
    # Page size in 1200-DPI units
    src_ps = src_pos["page_size"]
    reop_ps = reop_pos["page_size"]
    ps_delta_1200 = [abs(reop_ps["width"] - src_ps["width"]), abs(reop_ps["height"] - src_ps["height"])]
    ps_delta_pt = [ps_delta_1200[0] * 72.0 / 1200.0, ps_delta_1200[1] * 72.0 / 1200.0]

    # Elements geometry comparison
    src_els = src_pos["elements"]
    reop_els = reop_pos["elements"]
    
    count_equal = (len(src_els) == len(reop_els))
    
    deltas_x = []
    deltas_y = []
    deltas_sx = []
    deltas_sy = []
    max_deltas_per_el = []

    for i in range(min(len(src_els), len(reop_els))):
        e1 = src_els[i]
        e2 = reop_els[i]
        dx = abs(e2["x"] - e1["x"])
        dy = abs(e2["y"] - e1["y"])
        dsx = abs(e2["sx"] - e1["sx"])
        dsy = abs(e2["sy"] - e1["sy"])
        deltas_x.append(dx)
        deltas_y.append(dy)
        deltas_sx.append(dsx)
        deltas_sy.append(dsy)
        max_deltas_per_el.append(max(dx, dy, dsx, dsy))

    max_delta_1200 = max(max_deltas_per_el) if max_deltas_per_el else 0.0
    median_delta_1200 = statistics.median(max_deltas_per_el) if max_deltas_per_el else 0.0

    return {
        "src_page_size_1200": [src_ps["width"], src_ps["height"]],
        "reop_page_size_1200": [reop_ps["width"], reop_ps["height"]],
        "page_size_delta_1200": ps_delta_1200,
        "page_size_delta_pt": ps_delta_pt,
        "elements_count": [len(src_els), len(reop_els)],
        "count_equal": count_equal,
        "max_delta_1200": max_delta_1200,
        "median_delta_1200": median_delta_1200,
        "max_delta_pt": max_delta_1200 * 72.0 / 1200.0,
        "median_delta_pt": median_delta_1200 * 72.0 / 1200.0,
        "max_dx_1200": max(deltas_x) if deltas_x else 0.0,
        "max_dy_1200": max(deltas_y) if deltas_y else 0.0,
        "max_dsx_1200": max(deltas_sx) if deltas_sx else 0.0,
        "max_dsy_1200": max(deltas_sy) if deltas_sy else 0.0,
    }

async def run_roundtrip_for_target(session: ClientSession, doc: str, out_dir: Path, target_desc: str, score_option: dict | None = None) -> dict:
    opt_score = {"score": score_option} if score_option else {}
    
    # 1. Source capture
    # Positions (measure)
    src_pos_m_path = out_dir / f"{target_desc}_src_pos_m.json"
    await session.call_tool("score_export", {
        "document": doc, "kind": "positions",
        "options": {"position_kind": "measure", **opt_score},
        "destination": {"kind": "file", "path": str(src_pos_m_path.resolve())}
    })
    src_pos_m = json.loads(src_pos_m_path.read_text("utf-8"))

    # Positions (segment)
    src_pos_s_path = out_dir / f"{target_desc}_src_pos_s.json"
    await session.call_tool("score_export", {
        "document": doc, "kind": "positions",
        "options": {"position_kind": "segment", **opt_score},
        "destination": {"kind": "file", "path": str(src_pos_s_path.resolve())}
    })
    src_pos_s = json.loads(src_pos_s_path.read_text("utf-8"))

    # PDF render
    src_pdf_path = out_dir / f"{target_desc}_src.pdf"
    await session.call_tool("score_export", {
        "document": doc, "kind": "render",
        "options": {"format": "pdf", **opt_score},
        "destination": {"kind": "file", "path": str(src_pdf_path.resolve())}
    })
    src_pdf_desc = describe_pdf(src_pdf_path)

    # PNG render (page 0, 144 dpi)
    src_png_dir = out_dir / f"{target_desc}_src_png"
    src_png_dir.mkdir(parents=True, exist_ok=True)
    await session.call_tool("score_export", {
        "document": doc, "kind": "render",
        "options": {"format": "png", "dpi": 144, "first_page": 0, "last_page": 0, **opt_score},
        "destination": {"kind": "directory", "path": str(src_png_dir.resolve())}
    })
    src_png_file = next(src_png_dir.glob("page-*.png"))

    # 2. Export to MSCX and MSCZ
    results_by_format = {}
    for fmt in ("mscx", "mscz"):
        export_path = out_dir / f"{target_desc}.{fmt}"
        response = await session.call_tool("score_export", {
            "document": doc, "kind": "score",
            "options": {"format": fmt, "native_version": VERSION, **opt_score},
            "destination": {"kind": "file", "path": str(export_path.resolve())}
        })
        value = response.structured_content
        (out_dir / f"{target_desc}_{fmt}_response.json").write_text(json.dumps(value, indent=2))
        if not value["ok"]:
            assert value["error"]["code"] == "Unsupported", value
            assert not export_path.exists(), "refused export published output"
            results_by_format[fmt] = {"refused": value["error"]}
            continue
        if fmt == "mscz":
            with zipfile.ZipFile(export_path) as z:
                names = [n for n in z.namelist() if n.endswith(".mscx")]
                assert len(names) == 1, names
                xml = z.read(names[0])
        else:
            xml = export_path.read_bytes()
        assert ET.fromstring(xml).attrib["version"] == VERSION
        # Reopen
        reop_res = await session.call_tool("score_open", {"path": str(export_path.resolve()), "format": fmt})
        doc_reop = reop_res.structured_content["document"]

        # Reopened positions
        reop_pos_m_path = out_dir / f"{target_desc}_{fmt}_pos_m.json"
        await session.call_tool("score_export", {
            "document": doc_reop, "kind": "positions",
            "options": {"position_kind": "measure"},
            "destination": {"kind": "file", "path": str(reop_pos_m_path.resolve())}
        })
        reop_pos_m = json.loads(reop_pos_m_path.read_text("utf-8"))

        reop_pos_s_path = out_dir / f"{target_desc}_{fmt}_pos_s.json"
        await session.call_tool("score_export", {
            "document": doc_reop, "kind": "positions",
            "options": {"position_kind": "segment"},
            "destination": {"kind": "file", "path": str(reop_pos_s_path.resolve())}
        })
        reop_pos_s = json.loads(reop_pos_s_path.read_text("utf-8"))

        # Reopened PDF render
        reop_pdf_path = out_dir / f"{target_desc}_{fmt}.pdf"
        await session.call_tool("score_export", {
            "document": doc_reop, "kind": "render",
            "options": {"format": "pdf"},
            "destination": {"kind": "file", "path": str(reop_pdf_path.resolve())}
        })
        reop_pdf_desc = describe_pdf(reop_pdf_path)
        pdf_comparison = compare_vectors_pdf(src_pdf_path, reop_pdf_path)

        # Reopened PNG render
        reop_png_dir = out_dir / f"{target_desc}_{fmt}_png"
        reop_png_dir.mkdir(parents=True, exist_ok=True)
        await session.call_tool("score_export", {
            "document": doc_reop, "kind": "render",
            "options": {"format": "png", "dpi": 144, "first_page": 0, "last_page": 0},
            "destination": {"kind": "directory", "path": str(reop_png_dir.resolve())}
        })
        reop_png_file = next(reop_png_dir.glob("page-*.png"))
        png_comparison = compare_png(src_png_file, reop_png_file)

        await session.call_tool("score_close", {"document": doc_reop})

        comp_pos_m = compare_positions(src_pos_m, reop_pos_m)
        comp_pos_s = compare_positions(src_pos_s, reop_pos_s)

        within = (all(p["count_equal"] and p["max_delta_pt"] <= .02 and
                      max(p["page_size_delta_pt"]) <= .5 for p in (comp_pos_m, comp_pos_s))
                  and png_comparison["within_tolerance"] and pdf_comparison["text_equal"]
                  and pdf_comparison["drawings_counts"][0] == pdf_comparison["drawings_counts"][1]
                  and pdf_comparison["max_coord_delta_pt"] is not None
                  and pdf_comparison["max_coord_delta_pt"] <= .02
                  and max(pdf_comparison["rect_delta"]) <= .002)
        results_by_format[fmt] = {
            "within_012_tolerance": within,
            "measure_positions": comp_pos_m,
            "segment_positions": comp_pos_s,
            "pdf_descriptor": {
                "src": src_pdf_desc,
                "reopened": reop_pdf_desc,
                "comparison": pdf_comparison
            },
            "png_render": png_comparison
        }

    return results_by_format

async def run_single_iteration(run_id: str) -> dict:
    run_dir = PRECISION_DIR / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    params = StdioServerParameters(
        command=str(BINARY.resolve()),
        args=["--resources", str(RESOURCES.resolve()), "--soundfont", str(SOUNDFONT.resolve())],
        cwd=str(ROOT)
    )

    iteration_results = {}

    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            for item in FIXTURES:
                name = item["name"]
                fix_path = ROOT / item.get("folder", "fixtures/golden") / name
                fix_dir = run_dir / name
                fix_dir.mkdir(parents=True, exist_ok=True)

                print(f"[{run_id}] Processing fixture {name}...")
                open_res = await session.call_tool("score_open", {"path": str(fix_path.resolve()), "format": item["format"]})
                doc = open_res.structured_content["document"]

                master_res = await run_roundtrip_for_target(session, doc, fix_dir, "master")
                fix_entry = {"master": master_res}

                if item.get("has_excerpt"):
                    exc_id = item["excerpt_id"]
                    print(f"[{run_id}]   Processing excerpt {exc_id} for {name}...")
                    exc_res = await run_roundtrip_for_target(session, doc, fix_dir, f"excerpt_{exc_id}", score_option={"excerpt": exc_id})
                    fix_entry["excerpt"] = exc_res

                await session.call_tool("score_close", {"document": doc})
                iteration_results[name] = fix_entry

    return iteration_results

async def main():
    print("Starting writer/round-trip precision baseline measurement...")
    print(f"Binary: {BINARY}")
    print(f"Resources: {RESOURCES}")
    print(f"Soundfont: {SOUNDFONT}")

    # Run 1
    t0 = asyncio.get_event_loop().time()
    run1 = await run_single_iteration("run1")
    t1 = asyncio.get_event_loop().time()
    print(f"Run 1 completed in {t1 - t0:.2f} s")

    # Run 2
    run2 = await run_single_iteration("run2")
    t2 = asyncio.get_event_loop().time()
    print(f"Run 2 completed in {t2 - t1:.2f} s")

    # Check determinism between Run 1 and Run 2
    # Serialize to JSON and compare
    json1 = json.dumps(run1, sort_keys=True, indent=2)
    json2 = json.dumps(run2, sort_keys=True, indent=2)

    is_deterministic = (json1 == json2)
    repeatability = {}
    for fixture, targets in run1.items():
        for target, formats in targets.items():
            for fmt, result in formats.items():
                other = run2[fixture][target][fmt]
                key = f"{fixture}/{target}/{fmt}"
                repeatability[key] = {"exact_metrics_equal": result == other,
                    "outcomes_equal": ("refused" in result) == ("refused" in other)}
    print(f"Repeatability / Determinism (Run 1 == Run 2): {is_deterministic}")

    summary = {
        "provenance": provenance,
        "native_version": VERSION,
        "run2_results": run2,
        "binary": str(BINARY),
        "is_deterministic": is_deterministic,
        "repeatability": repeatability,
        "run1_seconds": t1 - t0,
        "run2_seconds": t2 - t1,
        "results": run1
    }

    results_path = PRECISION_DIR / "results.json"
    results_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Wrote full results to {results_path}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--host-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, help="compare a pinned measurement projection")
    args = parser.parse_args()
    BINARY, RESOURCES, SOUNDFONT, PRECISION_DIR = args.binary, args.resources, args.soundfont, args.output
    assert sha256_bytes(BINARY.read_bytes()) == args.host_sha256, "host hash mismatch"
    assert not PRECISION_DIR.exists(), "use a fresh output directory"
    PRECISION_DIR.mkdir(parents=True)
    # Accepted bounded-writer controls supplement the original representative corpus.
    for name in ("notes.mscx", "ties.mscx", "excerpts.mscx"):
        FIXTURES.append({"name": name, "folder": "tests/phase_b", "format": "mscx", "has_excerpt": name == "excerpts.mscx", "excerpt_id": "v_v"})
    provenance = {"platform": platform.platform(), "host_sha256": args.host_sha256,
        "host_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "engine_revision": subprocess.check_output(["git", "-C", "vendor/engrave", "rev-parse", "HEAD"], text=True).strip(),
        "sources": {str(Path(f.get("folder", "fixtures/golden"))/f["name"]): sha256_bytes((ROOT/f.get("folder", "fixtures/golden")/f["name"]).read_bytes()) for f in FIXTURES},
        "assets": {str(p.relative_to(RESOURCES)): sha256_bytes(p.read_bytes()) for p in sorted(RESOURCES.rglob("*")) if p.is_file()},
        "soundfont_sha256": sha256_bytes(SOUNDFONT.read_bytes())}
    (PRECISION_DIR/"preflight.json").write_text(json.dumps(provenance, indent=2))
    asyncio.run(main())
    if args.baseline:
        expected = json.loads(args.baseline.read_text(encoding="utf-8"))
        actual = json.loads((PRECISION_DIR/"results.json").read_text(encoding="utf-8"))
        for key in ("host_sha256", "engine_revision", "sources", "assets", "soundfont_sha256"):
            assert actual["provenance"][key] == expected["provenance"][key], f"baseline provenance mismatch: {key}"
        for run in ("results", "run2_results"):
            for fixture, targets in actual[run].items():
                for target, formats in targets.items():
                    for fmt, value in formats.items():
                        pinned = expected["measurements"][f"{fixture}/{target}/{fmt}"]
                        if "refused" in value:
                            error = value["refused"]
                            value = {"refused": error["code"], "warnings": [
                                [w["code"], w["object"]["kind"]] for w in error["export_warnings"]]}
                        assert value == pinned, f"baseline measurement mismatch: {fixture}/{target}/{fmt}"
        print("Pinned measurement projection matches both passes")
    manifest = {str(p.relative_to(PRECISION_DIR)): sha256_bytes(p.read_bytes()) for p in sorted(PRECISION_DIR.rglob("*")) if p.is_file()}
    (PRECISION_DIR/"manifest.json").write_text(json.dumps(manifest, indent=2))
