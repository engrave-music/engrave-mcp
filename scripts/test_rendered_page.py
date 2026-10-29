"""Geometry and mutation controls for the rendered-page oracle."""
from __future__ import annotations

import contextlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import check_rendered_page as page

ROOT = Path(__file__).resolve().parents[1]
PEDAL = ROOT / "tests/golden/windows/pedal.mscx/page-0.svg"
REFERENCE = ROOT / "tests/golden/page_reference.json"


class TestSvgGeometry(unittest.TestCase):
    def assertBoxAlmostEqual(self, actual, expected):
        self.assertIsNotNone(actual)
        for value, wanted in zip(actual, expected):
            self.assertAlmostEqual(value, wanted, places=6)

    def test_relative_commands_implicit_lines_and_close(self):
        self.assertBoxAlmostEqual(
            page.path_bounds("M10 10 l10 0 0 10 h-10 z m2 3 v4"),
            (10, 10, 20, 20))
        self.assertBoxAlmostEqual(page.path_bounds("M1e1,0 20 0 20 10z"), (10, 0, 20, 10))

    def test_curves_use_extrema_and_reset_reflection_after_line(self):
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 q5 10 10 0 t10 0"), (0, -5, 20, 5))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 Q5 10 10 0 L12 0 T20 0"), (0, 0, 20, 5))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 c0 12 12 12 12 0 s12 -12 12 0"),
                                  (0, -9, 24, 9))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 C0 12 12 12 12 0 L14 0 S20 0 24 0"),
                                  (0, 0, 24, 9))

    def test_arcs_include_rotation_sweep_large_arc_and_radius_correction(self):
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 a10 5 0 0 1 20 0"), (0, -5, 20, 0))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 A2 1 0 0 1 10 0"), (0, -2.5, 10, 0))
        self.assertBoxAlmostEqual(page.path_bounds("M10 0 A10 10 0 1 1 0 10"), (0, 0, 20, 20))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 A10 10 0 0120 0"), (0, -10, 20, 0))
        mat = page.parse_transform("translate(30 40) rotate(90)")
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 a10 5 0 0 1 20 0", mat),
                                  (30, 40, 35, 60))
        diagonal = 10 * math.sqrt(2)
        extent = math.sqrt(125)
        self.assertBoxAlmostEqual(
            page.path_bounds(f"M0 0 A10 5 45 0 1 {diagonal} {diagonal}"),
            (0, (10 - extent) / math.sqrt(2), (10 + extent) / math.sqrt(2), diagonal))

    def test_transform_list_and_ancestor_composition(self):
        mat = page.parse_transform("translate(10,20) scale(2 3) rotate(90)")
        x, y = page.transform_point((1, 2), mat)
        self.assertAlmostEqual(x, 6)
        self.assertAlmostEqual(y, 23)
        x, y = page.transform_point((1, 2), page.parse_transform("skewX(45) skewY(45)"))
        self.assertAlmostEqual(x, 4)
        self.assertAlmostEqual(y, 3)
        doc = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"
                    transform="translate(5 0)">
                   <g transform="translate(10 0)"><g transform="scale(2)">
                     <path transform="translate(3 0)" d="M0 0 l40 0"/>
                   </g></g></svg>"""
        stats = page.parse_svg_page(doc)
        self.assertEqual(stats.paths, 1)
        self.assertEqual(len(stats.bbox_violations), 1)
        self.assertIn("101.0", stats.bbox_violations[0])

    def test_nested_glyph_definition_transforms(self):
        doc = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 30">
                   <defs><g id="x" transform="translate(2 0)">
                     <g transform="scale(2)"><path d="M0 0 l4 0 0 4 z"/></g>
                   </g></defs><g transform="translate(18 0)"><use href="#x" x="3" y="0"/></g>
                 </svg>"""
        stats = page.parse_svg_page(doc)
        self.assertEqual(stats.total_uses, 1)
        self.assertEqual(len(stats.bbox_violations), 1)
        self.assertIn("31.0", stats.bbox_violations[0])

    def test_malformed_geometry_fails_closed(self):
        for data in ("M0 0 L1", "M0 0 Z1", "M0 0 A2 2 0 2 0 3 4", "M0 0 X1 2"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                page.path_bounds(data)
        with self.assertRaises(ValueError):
            page.parse_transform("translate(2) frobnicate(3)")
        with self.assertRaises(ValueError):
            page.parse_svg_page('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><use href="#missing"/></svg>')
        with self.assertRaises(ValueError):
            page.parse_svg_page('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><defs><g id="x"/></defs><use href="#x" x="nan"/></svg>')

    def test_zero_reference_has_no_ratio_and_nonzero_is_rejected(self):
        self.assertEqual(page.compare_stat(0, 0, 0.05), (None, None))
        ratio, issue = page.compare_stat(1, 0, 0.05)
        self.assertIsNone(ratio)
        self.assertIn("reference 0", issue)


class TestRenderedPageControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = PEDAL.read_text(encoding="utf-8")
        cls.reference = json.loads(REFERENCE.read_text(encoding="utf-8"))["pages"]["pedal.mscx/page-0.svg"]["stats"]

    def setUp(self):
        self.root = ET.fromstring(self.source)
        self.parents = {child: parent for parent in self.root.iter() for child in parent}

    def stats(self):
        return page.parse_svg_page(ET.tostring(self.root, encoding="unicode"))

    def test_unchanged_real_page_passes(self):
        self.assertEqual(page.page_issues(self.stats(), self.reference), [])

    def test_stacking_every_glyph_is_rejected_for_overlap(self):
        uses = list(self.root.iter(page.SVG + "use"))
        self.assertEqual(len(uses), 83)
        for use in uses:
            use.set("x", "250")
            use.set("y", "400")
        stats = self.stats()
        self.assertEqual(stats.bbox_violations, [])
        issues = page.page_issues(stats, self.reference)
        self.assertTrue(any(issue.startswith("overlap_pairs:") for issue in issues), issues)
        self.assertTrue(any(issue.startswith("overlap_area:") for issue in issues), issues)

    def test_ancestor_transform_moves_glyph_outside_viewbox(self):
        group = next(g for g in self.root.iter(page.SVG + "g") if g.find(page.SVG + "use") is not None)
        parent = self.parents[group]
        parent.remove(group)
        wrapper = ET.SubElement(parent, page.SVG + "g", {"transform": "translate(1000 0)"})
        wrapper.append(group)
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any(issue.startswith("glyph ") and "outside viewBox" in issue for issue in issues), issues)

    def test_translated_path_outside_viewbox_is_rejected(self):
        defs = {el for d in self.root.iter(page.SVG + "defs") for el in d.iter()}
        path = next(p for p in self.root.iter(page.SVG + "path") if p not in defs)
        path.set("transform", "translate(1000 0) " + path.get("transform", ""))
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any(issue.startswith("path ") and "outside viewBox" in issue for issue in issues), issues)

    def test_removed_glyph_group_is_rejected(self):
        group = next(g for g in self.root.iter(page.SVG + "g") if len(g.findall(page.SVG + "use")) >= 8)
        self.parents[group].remove(group)
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any(issue.startswith("total_uses:") for issue in issues), issues)
        self.assertTrue(any(issue.startswith("text_runs:") for issue in issues), issues)

    def test_cli_uses_checked_in_stats_and_rejects_translated_path(self):
        defs = {el for d in self.root.iter(page.SVG + "defs") for el in d.iter()}
        path = next(p for p in self.root.iter(page.SVG + "path") if p not in defs)
        with tempfile.TemporaryDirectory(prefix="rendered-page-") as temporary:
            temp = Path(temporary)
            target = temp / "pedal.mscx/page-0.svg"
            target.parent.mkdir()
            target.write_text(self.source, encoding="utf-8")
            payload = {"version": 2, "pages": {"pedal.mscx/page-0.svg": {"stats": self.reference}}}
            reference = temp / "reference.json"
            reference.write_text(json.dumps(payload), encoding="utf-8")
            args = ["check_rendered_page.py", "--golden", str(temp),
                    "--reference", str(reference)]
            with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(page.main(), 0)
            path.set("transform", "translate(1000 0) " + path.get("transform", ""))
            target.write_text(ET.tostring(self.root, encoding="unicode"), encoding="utf-8")
            output = io.StringIO()
            with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
                self.assertEqual(page.main(), 1)
            self.assertIn("outside viewBox", output.getvalue())


if __name__ == "__main__":
    unittest.main()
