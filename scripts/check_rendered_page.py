"""Rendered-page spot check without screenshots (roadmap A2 acceptance).

For every golden SVG:
  1. Verifies that all glyph and path bounding boxes lie strictly within the page viewBox.
  2. Compares per-page element counts by kind (paths, text glyph runs, notehead-like glyphs)
     against the 4.7.6 render in git (`codex/xmake-parity`), reporting ratios.
  3. Flags pages whose counts differ by more than documented thresholds.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]

# Documented thresholds for ratio differences (|actual / baseline - 1.0|):
# A2 rendering maintains exact layout parity with 4.7.6 for SVG vector graphics.
MAX_PATH_RATIO_DELTA = 0.05       # 5% max difference in drawn path count
MAX_TEXT_RUN_RATIO_DELTA = 0.05   # 5% max difference in text glyph runs
MAX_NOTEHEAD_RATIO_DELTA = 0.05   # 5% max difference in notehead glyphs
VIEWBOX_EPSILON = 0.01            # 0.01 pt margin for floating-point bounds rounding


@dataclass
class PageStats:
    paths: int
    text_runs: int
    noteheads: int
    total_uses: int
    bbox_violations: list[str]


def parse_matrix(transform_str: str | None) -> list[float] | None:
    if not transform_str or "matrix" not in transform_str:
        return None
    match = re.search(r"matrix\(([^)]+)\)", transform_str)
    if match:
        parts = [float(x.strip()) for x in match.group(1).split(",")]
        if len(parts) == 6:
            return parts
    return None


def transform_pt(x: float, y: float, mat: list[float] | None) -> tuple[float, float]:
    if not mat:
        return x, y
    a, b, c, d, e, f = mat
    return a * x + c * y + e, b * x + d * y + f


def parse_svg_page(svg_text: str) -> tuple[dict[str, float], PageStats]:
    root = ET.fromstring(svg_text)
    ns = "{http://www.w3.org/2000/svg}"
    
    viewbox_str = root.attrib.get("viewBox", "")
    vb_vals = [float(x) for x in viewbox_str.split()]
    if len(vb_vals) != 4:
        raise ValueError(f"Invalid or missing viewBox: {viewbox_str!r}")
    vx, vy, vw, vh = vb_vals
    v_min_x, v_max_x = vx, vx + vw
    v_min_y, v_max_y = vy, vy + vh
    viewbox = {"min_x": v_min_x, "max_x": v_max_x, "min_y": v_min_y, "max_y": v_max_y}

    defs = root.find(f"{ns}defs")
    defs_set = set(defs.iter()) if defs is not None else set()

    # Pre-parse glyph definitions: local bounding boxes and notehead classification
    glyph_bboxes: dict[str, tuple[float, float, float, float]] = {}
    glyph_is_notehead: dict[str, bool] = {}
    if defs is not None:
        for g in defs.iter(f"{ns}g"):
            gid = g.attrib.get("id")
            if not gid:
                continue
            path_el = g.find(f"{ns}path")
            d = path_el.attrib.get("d", "") if path_el is not None else ""
            nums = [float(x) for x in re.findall(r"[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?", d)]
            if nums and len(nums) >= 4:
                xs, ys = nums[0::2], nums[1::2]
                min_x, max_x = min(xs), max(xs)
                min_y, max_y = min(ys), max(ys)
                glyph_bboxes[gid] = (min_x, max_x, min_y, max_y)
                # Leland SMuFL standard notehead geometry: width 4.0-9.0 pt, height 2.5-7.5 pt,
                # vertically centered around baseline (min_y < 0 < max_y).
                w, h = max_x - min_x, max_y - min_y
                is_nh = (4.0 <= w <= 9.0) and (2.5 <= h <= 7.5) and (min_y < 0 < max_y)
                glyph_is_notehead[gid] = is_nh
            else:
                glyph_bboxes[gid] = (0.0, 0.0, 0.0, 0.0)
                glyph_is_notehead[gid] = False

    violations: list[str] = []

    # 1. Directly rendered paths (outside defs)
    rendered_paths = [el for el in root.iter(f"{ns}path") if el not in defs_set]
    for p_el in rendered_paths:
        mat = parse_matrix(p_el.attrib.get("transform"))
        d = p_el.attrib.get("d", "")
        nums = [float(x) for x in re.findall(r"[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?", d)]
        if nums and len(nums) >= 2:
            pts = [transform_pt(x, y, mat) for x, y in zip(nums[0::2], nums[1::2])]
            min_x = min(pt[0] for pt in pts)
            max_x = max(pt[0] for pt in pts)
            min_y = min(pt[1] for pt in pts)
            max_y = max(pt[1] for pt in pts)
            if (min_x < v_min_x - VIEWBOX_EPSILON or max_x > v_max_x + VIEWBOX_EPSILON or
                    min_y < v_min_y - VIEWBOX_EPSILON or max_y > v_max_y + VIEWBOX_EPSILON):
                violations.append(
                    f"path bounds [{min_x:.1f}, {max_x:.1f}, {min_y:.1f}, {max_y:.1f}] "
                    f"exceed viewBox [{v_min_x:.1f}, {v_max_x:.1f}, {v_min_y:.1f}, {v_max_y:.1f}]"
                )

    # 2. Uses and glyph runs
    text_runs = 0
    noteheads = 0
    total_uses = 0

    for g in root.iter(f"{ns}g"):
        if g in defs_set:
            continue
        g_mat = parse_matrix(g.attrib.get("transform"))
        uses = g.findall(f"{ns}use")
        if len(uses) > 1:
            text_runs += 1

        for u in uses:
            total_uses += 1
            href = u.attrib.get("{http://www.w3.org/1999/xlink}href", "").lstrip("#")
            if glyph_is_notehead.get(href, False):
                noteheads += 1

            u_mat = parse_matrix(u.attrib.get("transform"))
            mat = u_mat or g_mat
            ux = float(u.attrib.get("x", 0.0))
            uy = float(u.attrib.get("y", 0.0))
            g_bb = glyph_bboxes.get(href, (0.0, 0.0, 0.0, 0.0))
            corners = [
                (ux + g_bb[0], uy + g_bb[2]),
                (ux + g_bb[1], uy + g_bb[2]),
                (ux + g_bb[0], uy + g_bb[3]),
                (ux + g_bb[1], uy + g_bb[3]),
            ]
            tf_corners = [transform_pt(cx, cy, mat) for cx, cy in corners]
            min_x = min(pt[0] for pt in tf_corners)
            max_x = max(pt[0] for pt in tf_corners)
            min_y = min(pt[1] for pt in tf_corners)
            max_y = max(pt[1] for pt in tf_corners)
            if (min_x < v_min_x - VIEWBOX_EPSILON or max_x > v_max_x + VIEWBOX_EPSILON or
                    min_y < v_min_y - VIEWBOX_EPSILON or max_y > v_max_y + VIEWBOX_EPSILON):
                violations.append(
                    f"glyph {href} bounds [{min_x:.1f}, {max_x:.1f}, {min_y:.1f}, {max_y:.1f}] "
                    f"exceed viewBox [{v_min_x:.1f}, {v_max_x:.1f}, {v_min_y:.1f}, {v_max_y:.1f}]"
                )

    stats = PageStats(
        paths=len(rendered_paths),
        text_runs=text_runs,
        noteheads=noteheads,
        total_uses=total_uses,
        bbox_violations=violations,
    )
    return viewbox, stats


def get_baseline_svg_from_git(repo: Path, baseline_ref: str, rel_path: str) -> str | None:
    git_spec = f"{baseline_ref}:{rel_path}"
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo), "show", git_spec],
            capture_output=True, text=True, check=True
        )
        return proc.stdout
    except Exception:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, default=ROOT / "tests/golden/windows",
                        help="Golden root directory containing SVG files")
    parser.add_argument("--baseline-ref", default="codex/xmake-parity",
                        help="Git revision holding the 4.7.6 baseline SVG files")
    parser.add_argument("--repo", type=Path, default=ROOT,
                        help="Git repository path to query baseline files")
    args = parser.parse_args()

    golden_root = args.golden
    if not golden_root.is_dir():
        print(f"error: {golden_root} is not a directory", file=sys.stderr)
        return 2

    svg_files = sorted(golden_root.rglob("*.svg"))
    if not svg_files:
        print(f"error: no SVG files found in {golden_root}", file=sys.stderr)
        return 2

    print(f"Rendered-page spot check: {len(svg_files)} golden SVG pages against git {args.baseline_ref}")
    header = f"{'Page':<42} {'ViewBox Check':<14} {'Paths (5.0/4.7)':<18} {'Runs (5.0/4.7)':<18} {'Notes (5.0/4.7)':<18} {'Status'}"
    print(header)
    print("-" * len(header))

    failed = 0
    for svg_path in svg_files:
        rel_to_golden = svg_path.relative_to(golden_root).as_posix()
        # Full path in repo
        rel_in_repo = svg_path.relative_to(args.repo).as_posix() if svg_path.is_relative_to(args.repo) else f"tests/golden/windows/{rel_to_golden}"
        page_label = f"{svg_path.parent.name}/{svg_path.name}"
        if svg_path.parent.name.startswith("excerpt-"):
            page_label = f"{svg_path.parent.parent.name}/{svg_path.parent.name}/{svg_path.name}"

        curr_text = svg_path.read_text(encoding="utf-8")
        try:
            _, curr_stats = parse_svg_page(curr_text)
        except Exception as exc:
            print(f"{page_label:<42} ERROR: {exc}")
            failed += 1
            continue

        base_text = get_baseline_svg_from_git(args.repo, args.baseline_ref, rel_in_repo)
        if base_text is None:
            # Try alternate path relative to tests/golden/windows
            alt_path = f"tests/golden/windows/{rel_to_golden}"
            base_text = get_baseline_svg_from_git(args.repo, args.baseline_ref, alt_path)

        if base_text is None:
            print(f"{page_label:<42} MISSING BASELINE in git {args.baseline_ref}")
            failed += 1
            continue

        _, base_stats = parse_svg_page(base_text)

        # Ratios
        rp = curr_stats.paths / base_stats.paths if base_stats.paths else 1.0
        rt = curr_stats.text_runs / base_stats.text_runs if base_stats.text_runs else 1.0
        rn = curr_stats.noteheads / base_stats.noteheads if base_stats.noteheads else 1.0

        p_str = f"{curr_stats.paths:3d}/{base_stats.paths:3d} ({rp:.2f})"
        t_str = f"{curr_stats.text_runs:2d}/{base_stats.text_runs:2d} ({rt:.2f})"
        n_str = f"{curr_stats.noteheads:3d}/{base_stats.noteheads:3d} ({rn:.2f})"

        bbox_ok = len(curr_stats.bbox_violations) == 0
        vb_str = "OK (in bounds)" if bbox_ok else f"FAIL ({len(curr_stats.bbox_violations)})"

        page_issues = []
        if not bbox_ok:
            page_issues.append(f"{len(curr_stats.bbox_violations)} element(s) outside viewBox")
        if abs(rp - 1.0) > MAX_PATH_RATIO_DELTA:
            page_issues.append(f"paths ratio {rp:.2f} exceeds threshold {MAX_PATH_RATIO_DELTA:.2f}")
        if abs(rt - 1.0) > MAX_TEXT_RUN_RATIO_DELTA:
            page_issues.append(f"text runs ratio {rt:.2f} exceeds threshold {MAX_TEXT_RUN_RATIO_DELTA:.2f}")
        if abs(rn - 1.0) > MAX_NOTEHEAD_RATIO_DELTA:
            page_issues.append(f"noteheads ratio {rn:.2f} exceeds threshold {MAX_NOTEHEAD_RATIO_DELTA:.2f}")

        status = "PASS" if not page_issues else "FAIL"
        if page_issues:
            failed += 1

        print(f"{page_label:<42} {vb_str:<14} {p_str:<18} {t_str:<18} {n_str:<18} {status}")
        for iss in page_issues:
            print(f"    Issue: {iss}")

    print("-" * len(header))
    print(f"Total: {len(svg_files)} pages checked, {failed} failure(s)")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
