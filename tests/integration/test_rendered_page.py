"""Geometry and mutation controls for the rendered-page oracle."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import contextlib
import hashlib
import io
import json
import math
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw
from PIL.PngImagePlugin import PngInfo
from support import check_rendered_page as page
from support.paths import REPO_ROOT

ROOT = REPO_ROOT
PEDAL = ROOT / "tests/data/golden/windows/pedal.mscx/page-0.svg"
BEND = ROOT / "tests/data/golden/windows/bend.gp/page-0.svg"
LAYOUT = ROOT / "tests/data/golden/windows/layout_elements.mscx/page-0.svg"
REFERENCE = ROOT / "tests/data/golden/page_reference.json"


class TestSvgGeometry(unittest.TestCase):
    def assertBoxAlmostEqual(self, actual, expected):
        self.assertIsNotNone(actual)
        for value, wanted in zip(actual, expected):
            self.assertAlmostEqual(value, wanted, places=6)

    def test_relative_commands_implicit_lines_and_close(self):
        self.assertBoxAlmostEqual(page.path_bounds("M10 10 l10 0 0 10 h-10 z m2 3 v4"), (10, 10, 20, 20))
        self.assertBoxAlmostEqual(page.path_bounds("M1e1,0 20 0 20 10z"), (10, 0, 20, 10))

    def test_curves_use_extrema_and_reset_reflection_after_line(self):
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 q5 10 10 0 t10 0"), (0, -5, 20, 5))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 Q5 10 10 0 L12 0 T20 0"), (0, 0, 20, 5))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 c0 12 12 12 12 0 s12 -12 12 0"), (0, -9, 24, 9))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 C0 12 12 12 12 0 L14 0 S20 0 24 0"), (0, 0, 24, 9))

    def test_arcs_include_rotation_sweep_large_arc_and_radius_correction(self):
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 a10 5 0 0 1 20 0"), (0, -5, 20, 0))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 A2 1 0 0 1 10 0"), (0, -2.5, 10, 0))
        self.assertBoxAlmostEqual(page.path_bounds("M10 0 A10 10 0 1 1 0 10"), (0, 0, 20, 20))
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 A10 10 0 0120 0"), (0, -10, 20, 0))
        mat = page.parse_transform("translate(30 40) rotate(90)")
        self.assertBoxAlmostEqual(page.path_bounds("M0 0 a10 5 0 0 1 20 0", mat), (30, 40, 35, 60))
        diagonal = 10 * math.sqrt(2)
        extent = math.sqrt(125)
        self.assertBoxAlmostEqual(
            page.path_bounds(f"M0 0 A10 5 45 0 1 {diagonal} {diagonal}"),
            (0, (10 - extent) / math.sqrt(2), (10 + extent) / math.sqrt(2), diagonal),
        )

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
            page.parse_svg_page(
                '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><use href="#missing"/></svg>'
            )
        with self.assertRaises(ValueError):
            page.parse_svg_page(
                '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><defs><g id="x"/></defs><use href="#x" x="nan"/></svg>'
            )

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
        self.assertTrue(
            any(issue.startswith("glyph ") and "outside viewBox" in issue for issue in issues),
            issues,
        )

    def test_translated_path_outside_viewbox_is_rejected(self):
        defs = {el for d in self.root.iter(page.SVG + "defs") for el in d.iter()}
        path = next(p for p in self.root.iter(page.SVG + "path") if p not in defs)
        path.set("transform", "translate(1000 0) " + path.get("transform", ""))
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(
            any(issue.startswith("path ") and "outside viewBox" in issue for issue in issues),
            issues,
        )

    def test_removed_glyph_group_is_rejected(self):
        group = next(g for g in self.root.iter(page.SVG + "g") if len(g.findall(page.SVG + "use")) >= 8)
        self.parents[group].remove(group)
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any(issue.startswith("total_uses:") for issue in issues), issues)
        self.assertTrue(any(issue.startswith("text_runs:") for issue in issues), issues)

    def test_reviewer_scaled_every_bend_use_to_near_zero(self):
        root = ET.fromstring(BEND.read_text(encoding="utf-8"))
        for use in root.iter(page.SVG + "use"):
            use.set("transform", "translate(200 200) scale(0.000001)")
        reference = json.loads(REFERENCE.read_text(encoding="utf-8"))["pages"]["bend.gp/page-0.svg"]["stats"]
        issues = page.page_issues(page.parse_svg_page(ET.tostring(root, encoding="unicode")), reference)
        self.assertTrue(any("glyph minimum scale" in issue for issue in issues), issues)

    def test_one_shrunken_glyph_is_rejected(self):
        use = next(self.root.iter(page.SVG + "use"))
        use.set("transform", "translate(200 200) scale(0.01)")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any("glyph minimum scale" in issue for issue in issues), issues)

    def test_scale_inside_a_glyph_definition_is_rejected(self):
        definitions = {el.get("id"): el for el in self.root.iter() if el.get("id")}
        for use in self.root.iter(page.SVG + "use"):
            name = (use.get("href") or use.get(page.XLINK + "href"))[1:]
            path = definitions[name].find(".//" + page.SVG + "path")
            if path is not None:
                path.set("transform", "scale(0.000001)")
                break
        else:
            self.fail("fixture has no painted glyph definition")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any("glyph minimum scale" in issue for issue in issues), issues)

    def test_reviewer_root_opacity_zero(self):
        self.root.set("opacity", "0")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(any("hidden by opacity 0" in issue for issue in issues), issues)

    def test_hidden_ancestor_and_style_are_rejected(self):
        group = next(g for g in self.root.iter(page.SVG + "g") if g.find(page.SVG + "use") is not None)
        for attribute, value in (
            ("display", "none"),
            ("visibility", "hidden"),
            ("style", "opacity: 0% !important"),
        ):
            with self.subTest(attribute=attribute):
                group.set(attribute, value)
                issues = page.page_issues(self.stats(), self.reference)
                self.assertTrue(any("hidden by" in issue for issue in issues), issues)
                del group.attrib[attribute]

    def test_hidden_definition_ancestor_is_rejected(self):
        group = self.root.find(page.SVG + "defs").find(page.SVG + "g")
        group.set("visibility", "hidden")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(
            any("glyph " in issue and "visibility hidden" in issue for issue in issues),
            issues,
        )

    def test_unpainted_path_and_glyph_ancestor_are_rejected(self):
        defs = {el for d in self.root.iter(page.SVG + "defs") for el in d.iter()}
        path = next(
            p
            for p in self.root.iter(page.SVG + "path")
            if p not in defs and p.get("fill") == "none" and p.get("stroke")
        )
        original = path.get("stroke")
        path.set("stroke", "none")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(
            any("path has no visible fill or stroke" in issue for issue in issues),
            issues,
        )
        path.set("stroke", original)
        group = next(g for g in self.root.iter(page.SVG + "g") if g.find(page.SVG + "use") is not None)
        group.set("style", "fill:none; stroke:none")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(
            any("glyph " in issue and "no visible fill or stroke" in issue for issue in issues),
            issues,
        )

    def test_zero_fill_and_stroke_opacity_are_rejected(self):
        group = next(g for g in self.root.iter(page.SVG + "g") if g.find(page.SVG + "use") is not None)
        group.set("style", "fill-opacity:0; stroke-opacity:0")
        issues = page.page_issues(self.stats(), self.reference)
        self.assertTrue(
            any("glyph " in issue and "no visible fill or stroke" in issue for issue in issues),
            issues,
        )

    def test_reviewer_removed_one_layout_glyph(self):
        root = ET.fromstring(LAYOUT.read_text(encoding="utf-8"))
        parents = {child: parent for parent in root.iter() for child in parent}
        use = next(root.iter(page.SVG + "use"))
        parents[use].remove(use)
        reference = json.loads(REFERENCE.read_text(encoding="utf-8"))["pages"]["layout_elements.mscx/page-0.svg"][
            "stats"
        ]
        issues = page.page_issues(page.parse_svg_page(ET.tostring(root, encoding="unicode")), reference)
        self.assertTrue(any(issue.startswith("total_uses:") for issue in issues), issues)

    def test_cli_uses_checked_in_stats_and_rejects_translated_path(self):
        defs = {el for d in self.root.iter(page.SVG + "defs") for el in d.iter()}
        path = next(p for p in self.root.iter(page.SVG + "path") if p not in defs)
        with tempfile.TemporaryDirectory(prefix="rendered-page-") as temporary:
            temp = Path(temporary)
            target = temp / "pedal.mscx/page-0.svg"
            target.parent.mkdir()
            target.write_text(self.source, encoding="utf-8")
            payload = {
                "version": 3,
                "pages": {"pedal.mscx/page-0.svg": {"stats": self.reference, "png": None}},
            }
            reference = temp / "reference.json"
            reference.write_text(json.dumps(payload), encoding="utf-8")
            args = [
                "check_rendered_page.py",
                "--golden",
                str(temp),
                "--reference",
                str(reference),
            ]
            with (
                mock.patch.object(sys, "argv", args),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(page.main(), 0)
            path.set("transform", "translate(1000 0) " + path.get("transform", ""))
            target.write_text(ET.tostring(self.root, encoding="unicode"), encoding="utf-8")
            output = io.StringIO()
            with (
                mock.patch.object(sys, "argv", args),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(page.main(), 1)
            self.assertIn("outside viewBox", output.getvalue())

    def test_reviewed_svg_hash_is_exact_and_platform_specific(self):
        name = "layout_elements.mscx/page-0.svg"
        index = json.loads((page.ROOT / "tests/data/golden/linux/index.json").read_text(encoding="utf-8"))
        digest = index["layout_elements.mscx"]["page-0.svg"]
        with mock.patch.object(page.sys, "platform", "linux"):
            self.assertTrue(page.accepted_svg_counts(name, digest))
            self.assertFalse(page.accepted_svg_counts(name, "0" * 64))
            self.assertFalse(page.accepted_svg_counts("unknown.mscx/page-0.svg", digest))
        with mock.patch.object(page.sys, "platform", "win32"):
            self.assertFalse(page.accepted_svg_counts(name, digest))

    def test_removed_glyph_with_updated_index_fails(self):
        name = "layout_elements.mscx/page-0.svg"
        platform = "windows" if sys.platform.startswith("win") else "linux"
        source = page.ROOT / "tests/data/golden" / platform / name
        payload = json.loads((page.ROOT / "tests/data/golden/page_reference.json").read_text())
        payload["pages"] = {name: payload["pages"][name]}
        payload["pages"][name]["png"] = None
        payload["png_exceptions"] = []
        payload["count_exceptions"] = [e for e in payload["count_exceptions"] if e["page"] == name]
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            target = temp / "capture" / name
            target.parent.mkdir(parents=True)
            target.write_bytes(source.read_bytes())
            reference = temp / "reference.json"
            reference.write_text(json.dumps(payload))
            args = ["checker", "--golden", str(temp / "capture"), "--reference", str(reference)]
            with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(page.main(), 0)
            tree = ET.fromstring(target.read_text())
            use = next(tree.iter(page.SVG + "use"))
            parents = {child: parent for parent in tree.iter() for child in parent}
            parents[use].remove(use)
            target.write_text(ET.tostring(tree, encoding="unicode"))
            fake = temp / "repo"
            baseline = fake / "tests/data/golden" / platform / name
            baseline.parent.mkdir(parents=True)
            baseline.write_bytes(target.read_bytes())
            (baseline.parent.parent / "index.json").write_text(
                json.dumps({"layout_elements.mscx": {"page-0.svg": hashlib.sha256(target.read_bytes()).hexdigest()}})
            )
            with (
                mock.patch.object(page, "ROOT", fake),
                mock.patch.object(sys, "argv", args),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(page.main(), 1)

    def test_count_exception_requires_exact_changed_counts_and_svg_hash(self):
        use = next(self.root.iter(page.SVG + "use"))
        self.parents[use].remove(use)
        source = ET.tostring(self.root, encoding="unicode")
        stats = page.parse_svg_page(source)
        counts = {
            name: getattr(stats, name)
            for name in ("paths", "text_runs", "noteheads", "total_uses")
            if getattr(stats, name) != self.reference[name]
        }
        with tempfile.TemporaryDirectory(prefix="rendered-count-") as temporary:
            temp = Path(temporary)
            target = temp / "pedal.mscx/page-0.svg"
            target.parent.mkdir()
            target.write_text(source, encoding="utf-8", newline="\n")
            entry = {
                "page": "pedal.mscx/page-0.svg",
                "counts": counts,
                "svg_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "reason": "control",
            }
            payload = {
                "version": 3,
                "pages": {entry["page"]: {"stats": self.reference, "png": None}},
                "count_exceptions": [entry],
            }
            reference = temp / "reference.json"
            args = [
                "check_rendered_page.py",
                "--golden",
                str(temp),
                "--reference",
                str(reference),
            ]
            reference.write_text(json.dumps(payload), encoding="utf-8")
            with (
                mock.patch.object(sys, "argv", args),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(page.main(), 0)
            target.write_text(source + "\n", encoding="utf-8", newline="\n")
            output = io.StringIO()
            with (
                mock.patch.object(sys, "argv", args),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(page.main(), 1)
            self.assertIn("SHA-256 does not match", output.getvalue())


class TestPngInkControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rendered-png-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.reference_path = self.root / "reference.png"
        image = Image.new("RGBA", (128, 128), "white")
        draw = ImageDraw.Draw(image)
        for box in (
            (14, 18, 114, 20),
            (14, 62, 114, 64),
            (14, 108, 114, 110),
            (24, 12, 27, 116),
            (96, 12, 99, 116),
        ):
            draw.rectangle(box, fill="black")
        image.save(self.reference_path)
        self.reference = page.png_stats(self.reference_path)

    def issues_for(self, image):
        path = self.root / "actual.png"
        image.save(path)
        return page.png_issues(page.png_stats(path), self.reference)

    def test_blank_png_is_rejected(self):
        issues = self.issues_for(Image.new("RGBA", (128, 128), "white"))
        self.assertTrue(any("ink share" in issue for issue in issues), issues)
        self.assertTrue(any("no ink bounding box" in issue for issue in issues), issues)
        self.assertTrue(any("grid L1" in issue for issue in issues), issues)

    def test_changed_dimensions_do_not_mask_missing_ink(self):
        issues = self.issues_for(Image.new("RGBA", (127, 128), "white"))
        self.assertTrue(any("PNG size" in issue for issue in issues), issues)
        self.assertTrue(any("ink share" in issue for issue in issues), issues)
        self.assertTrue(any("grid L1" in issue for issue in issues), issues)

    def test_fully_transparent_black_png_is_blank(self):
        issues = self.issues_for(Image.new("RGBA", (128, 128), (0, 0, 0, 0)))
        self.assertTrue(any("no ink bounding box" in issue for issue in issues), issues)

    def test_content_shrunk_to_one_corner_is_rejected(self):
        with Image.open(self.reference_path) as original:
            small = original.resize((32, 32), Image.Resampling.NEAREST)
        canvas = Image.new("RGBA", (128, 128), "white")
        canvas.paste(small, (0, 0))
        issues = self.issues_for(canvas)
        for message in ("ink share", "bbox edge", "grid L1"):
            self.assertTrue(any(message in issue for issue in issues), issues)

    def test_png_stats_composite_alpha_and_measure_grid(self):
        image = Image.new("RGBA", (16, 16), "white")
        image.putpixel((0, 0), (0, 0, 0, 255))
        image.putpixel((15, 15), (0, 0, 0, 255))
        image.putpixel((3, 3), (0, 0, 0, 0))
        path = self.root / "points.png"
        image.save(path)
        stats = page.png_stats(path)
        self.assertEqual(stats["ink_share"], 2 / 256)
        self.assertEqual(stats["bbox"], [0, 0, 16, 16])
        self.assertEqual(stats["grid_density"][0][0], 0.25)
        self.assertEqual(stats["grid_density"][7][7], 0.25)

    def test_cli_requires_every_expected_png_and_rejects_extras(self):
        target = self.root / "pedal.mscx/page-0.svg"
        target.parent.mkdir()
        target.write_text(PEDAL.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        reference = json.loads(REFERENCE.read_text(encoding="utf-8"))["pages"]["pedal.mscx/page-0.svg"]
        payload = {
            "version": 3,
            "pages": {
                "pedal.mscx/page-0.svg": {
                    "stats": reference["stats"],
                    "png": {"stats": self.reference},
                }
            },
        }
        refpath = self.root / "reference.json"
        refpath.write_text(json.dumps(payload), encoding="utf-8")
        args = [
            "check_rendered_page.py",
            "--golden",
            str(self.root),
            "--reference",
            str(refpath),
        ]
        # Keep synthetic source PNGs outside the captured tree's inventory.
        self.reference_path.unlink()
        output = io.StringIO()
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
            self.assertEqual(page.main(), 1)
        self.assertIn("reference PNG missing", output.getvalue())
        image = Image.new("RGBA", (128, 128), "white")
        draw = ImageDraw.Draw(image)
        for box in (
            (14, 18, 114, 20),
            (14, 62, 114, 64),
            (14, 108, 114, 110),
            (24, 12, 27, 116),
            (96, 12, 99, 116),
        ):
            draw.rectangle(box, fill="black")
        image.save(target.with_suffix(".png"))
        with (
            mock.patch.object(sys, "argv", args),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(page.main(), 0)
        image.save(self.root / "unexpected.png")
        output = io.StringIO()
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
            self.assertEqual(page.main(), 1)
        self.assertIn("PNG has no reference", output.getvalue())

    def test_precision_exception_requires_exact_png_bytes_and_diagnostics(self):
        target = self.root / "pedal.mscx/page-0.svg"
        target.parent.mkdir()
        target.write_text(PEDAL.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        svg_reference = json.loads(REFERENCE.read_text(encoding="utf-8"))["pages"]["pedal.mscx/page-0.svg"]
        image = Image.new("RGBA", (128, 128), "white")
        png = target.with_suffix(".png")
        image.save(png)
        issues = page.png_issues(page.png_stats(png), self.reference)
        pin = {
            "page": "pedal.mscx/page-0.png",
            "platform": "windows" if sys.platform.startswith("win") else "linux",
            "png_sha256": hashlib.sha256(png.read_bytes()).hexdigest(),
            "issues": issues,
            "reason": "synthetic precision control",
        }
        payload = {
            "version": 3,
            "pages": {
                "pedal.mscx/page-0.svg": {
                    "stats": svg_reference["stats"],
                    "png": {"stats": self.reference},
                }
            },
            "png_exceptions": [pin],
        }
        refpath = self.root / "reference.json"
        refpath.write_text(json.dumps(payload), encoding="utf-8")
        self.reference_path.unlink()
        args = [
            "check_rendered_page.py",
            "--golden",
            str(self.root),
            "--reference",
            str(refpath),
        ]
        with (
            mock.patch.object(sys, "argv", args),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(page.main(), 0)
        # Add a PNG metadata chunk without changing any pixels: even that byte
        # change must invalidate this particular page's exception.
        metadata = PngInfo()
        metadata.add_text("changed", "yes")
        image.save(png, pnginfo=metadata)
        self.assertEqual(page.png_issues(page.png_stats(png), self.reference), issues)
        output = io.StringIO()
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
            self.assertEqual(page.main(), 1)
        self.assertIn("precision exception PNG SHA-256 does not match", output.getvalue())
        pin["png_sha256"] = hashlib.sha256(png.read_bytes()).hexdigest()
        pin["issues"] = ["different diagnostic"]
        refpath.write_text(json.dumps(payload), encoding="utf-8")
        output = io.StringIO()
        with mock.patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
            self.assertEqual(page.main(), 1)
        self.assertIn("precision exception diagnostics do not match", output.getvalue())


if __name__ == "__main__":
    unittest.main()
