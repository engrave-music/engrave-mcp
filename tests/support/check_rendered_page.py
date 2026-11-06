"""Compare exported SVG and PNG pages with immutable 4.7.6 statistics.

This is a geometry/content spot check, not a visual proof of legibility. Path
bounds are centerline/fill geometry (stroke width and clipping are not modeled).
Glyph overlap is measured between transformed outline bounding boxes.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import hashlib
import json
import math
import os
import re
import statistics
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

from support.paths import REPO_ROOT

ROOT = REPO_ROOT
SVG = "{http://www.w3.org/2000/svg}"
XLINK = "{http://www.w3.org/1999/xlink}"
IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
NUMBER = re.compile(r"[-+]?(?:(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)")

# The 10% overlap margins allow small glyph-box changes while detecting substantial
# added collisions. Element counts are exact, except for a SHA-pinned exception.
OVERLAP_DELTA = 0.10
MIN_GLYPH_SCALE_RATIO = 0.5
# Measured across the 22 paired 4.7.6/5.0 master PNGs: ink ratios are
# 0.9665..1.0272, the largest normalized bbox edge movement is 0.0714, and
# the largest grid L1 distance is 0.1909. These bounds leave room for reflow.
INK_RATIO_DELTA = 0.10
BBOX_EDGE_DELTA = 0.09
GRID_L1_LIMIT = 0.25
INK_THRESHOLD = 250  # composite over white; RGB below this is visible ink
GRID_SIZE = 8
VIEWBOX_EPSILON = 0.01  # points, for SVG coordinate rounding
OVERLAP_EPSILON = 0.05  # points in each axis, excluding incidental box contact


@dataclass
class PageStats:
    viewbox: list[float]
    paths: int
    text_runs: int
    noteheads: int
    total_uses: int
    overlap_pairs: int
    overlap_area: float
    glyph_min_scales: dict[str, float] = field(default_factory=dict)
    glyph_median_sizes: dict[str, list[float]] = field(default_factory=dict)
    bbox_violations: list[str] = field(default_factory=list)
    visibility_violations: list[str] = field(default_factory=list)


def compose(a, b):
    """Compose SVG affine matrices as a * b, so b acts on a point first."""
    a0, a1, a2, a3, a4, a5 = a
    b0, b1, b2, b3, b4, b5 = b
    return (
        a0 * b0 + a2 * b1,
        a1 * b0 + a3 * b1,
        a0 * b2 + a2 * b3,
        a1 * b2 + a3 * b3,
        a0 * b4 + a2 * b5 + a4,
        a1 * b4 + a3 * b5 + a5,
    )


def transform_point(point, mat):
    x, y = point
    a, b, c, d, e, f = mat
    return a * x + c * y + e, b * x + d * y + f


class Scanner:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def skip(self):
        while self.pos < len(self.text) and (self.text[self.pos].isspace() or self.text[self.pos] == ","):
            self.pos += 1

    def number(self):
        self.skip()
        match = NUMBER.match(self.text, self.pos)
        if not match:
            raise ValueError(f"expected SVG number at {self.text[self.pos : self.pos + 20]!r}")
        self.pos = match.end()
        value = float(match.group())
        if not math.isfinite(value):
            raise ValueError("non-finite SVG number")
        return value

    def flag(self):
        # Arc flags are single characters and may be adjacent to one another
        # and to the following coordinate (e.g. A 5 5 0 0110 20).
        self.skip()
        if self.pos == len(self.text) or self.text[self.pos] not in "01":
            raise ValueError("invalid SVG arc flag")
        value = int(self.text[self.pos])
        self.pos += 1
        return value


def parse_transform(text):
    if not text:
        return IDENTITY
    result = IDENTITY
    pos = 0
    while pos < len(text):
        match = re.match(r"[\s,]*([A-Za-z]+)\s*\(([^()]*)\)", text[pos:])
        if not match:
            if not text[pos:].strip(" \t\r\n,"):
                break
            raise ValueError(f"invalid SVG transform {text[pos:]!r}")
        name, arguments = match.groups()
        scan = Scanner(arguments)
        values = []
        while True:
            scan.skip()
            if scan.pos == len(arguments):
                break
            values.append(scan.number())
        if name == "matrix" and len(values) == 6:
            matrix = tuple(values)
        elif name == "translate" and len(values) in (1, 2):
            matrix = (1, 0, 0, 1, values[0], values[1] if len(values) == 2 else 0)
        elif name == "scale" and len(values) in (1, 2):
            matrix = (values[0], 0, 0, values[-1], 0, 0)
        elif name == "rotate" and len(values) in (1, 3):
            angle = math.radians(values[0])
            c, s = math.cos(angle), math.sin(angle)
            matrix = (c, s, -s, c, 0, 0)
            if len(values) == 3:
                cx, cy = values[1:]
                matrix = compose(compose((1, 0, 0, 1, cx, cy), matrix), (1, 0, 0, 1, -cx, -cy))
        elif name in ("skewX", "skewY") and len(values) == 1:
            tangent = math.tan(math.radians(values[0]))
            matrix = (1, 0, tangent, 1, 0, 0) if name == "skewX" else (1, tangent, 0, 1, 0, 0)
        else:
            raise ValueError(f"unsupported or invalid SVG transform {name}({arguments})")
        result = compose(result, matrix)
        pos += match.end()
    return result


@lru_cache(maxsize=8192)
def path_segments(text):
    """Parse all SVG path commands into absolute line, curve, and arc segments."""
    scan = Scanner(text)
    segments = []
    current = (0.0, 0.0)
    start = current
    previous = ""
    control = current
    command = None

    def point(relative):
        x, y = scan.number(), scan.number()
        return (x + current[0], y + current[1]) if relative else (x, y)

    while True:
        scan.skip()
        if scan.pos == len(text):
            break
        char = text[scan.pos]
        if char.isalpha():
            if char not in "MmLlHhVvCcSsQqTtAaZz":
                raise ValueError(f"unsupported SVG path command {char!r}")
            command = char
            scan.pos += 1
            if previous == "" and command not in "Mm":
                raise ValueError("SVG path must begin with moveto")
            if command in "Zz":
                segments.append(("L", current, start))
                current = start
                previous = "Z"
                command = None
                continue
        elif command is None:
            raise ValueError(f"expected SVG path command at {text[scan.pos : scan.pos + 20]!r}")

        relative = command.islower()
        kind = command.upper()
        if kind == "M":
            current = point(relative)
            start = current
            command = "l" if relative else "L"
        elif kind == "L":
            end = point(relative)
            segments.append(("L", current, end))
            current = end
        elif kind == "H":
            value = scan.number() + (current[0] if relative else 0)
            end = (value, current[1])
            segments.append(("L", current, end))
            current = end
        elif kind == "V":
            value = scan.number() + (current[1] if relative else 0)
            end = (current[0], value)
            segments.append(("L", current, end))
            current = end
        elif kind == "C":
            c1, c2, end = point(relative), point(relative), point(relative)
            segments.append(("C", current, c1, c2, end))
            current, control = end, c2
        elif kind == "S":
            c1 = (2 * current[0] - control[0], 2 * current[1] - control[1]) if previous in ("C", "S") else current
            c2, end = point(relative), point(relative)
            segments.append(("C", current, c1, c2, end))
            current, control = end, c2
        elif kind == "Q":
            c1, end = point(relative), point(relative)
            segments.append(("Q", current, c1, end))
            current, control = end, c1
        elif kind == "T":
            c1 = (2 * current[0] - control[0], 2 * current[1] - control[1]) if previous in ("Q", "T") else current
            end = point(relative)
            segments.append(("Q", current, c1, end))
            current, control = end, c1
        elif kind == "A":
            rx, ry, angle = scan.number(), scan.number(), scan.number()
            large, sweep = scan.flag(), scan.flag()
            end = point(relative)
            if rx < 0 or ry < 0:
                raise ValueError("negative SVG arc radius")
            segments.append(("A", current, end, rx, ry, angle, large, sweep))
            current = end
        previous = kind
    return tuple(segments)


def _quadratic_roots(a, b, c):
    if abs(a) < 1e-14:
        return [] if abs(b) < 1e-14 else [-c / b]
    disc = b * b - 4 * a * c
    if disc < 0:
        return []
    root = math.sqrt(disc)
    return [(-b + root) / (2 * a), (-b - root) / (2 * a)]


def _curve_points(points):
    """Return endpoints and the extrema of a transformed quadratic/cubic."""
    found = [points[0], points[-1]]
    ts = []
    for axis in (0, 1):
        p = [value[axis] for value in points]
        if len(p) == 3:
            denominator = p[0] - 2 * p[1] + p[2]
            if abs(denominator) > 1e-14:
                ts.append((p[0] - p[1]) / denominator)
        else:
            a = -p[0] + 3 * p[1] - 3 * p[2] + p[3]
            b = 3 * p[0] - 6 * p[1] + 3 * p[2]
            c = 3 * (p[1] - p[0])
            ts.extend(_quadratic_roots(3 * a, 2 * b, c))
    for t in ts:
        if 0 < t < 1:
            work = list(points)
            while len(work) > 1:
                work = [((1 - t) * a[0] + t * b[0], (1 - t) * a[1] + t * b[1]) for a, b in zip(work, work[1:])]
            found.append(work[0])
    return found


def _arc_points(start, end, rx, ry, rotation, large, sweep, mat):
    """SVG endpoint-form elliptical arc, including radii correction and extrema."""
    p0, p1 = transform_point(start, mat), transform_point(end, mat)
    if start == end or rx == 0 or ry == 0:
        return [p0, p1]
    phi = math.radians(rotation % 360)
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (start[0] - end[0]) / 2, (start[1] - end[1]) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    scale = xp * xp / (rx * rx) + yp * yp / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    denominator = rx * rx * yp * yp + ry * ry * xp * xp
    numerator = max(0.0, rx * rx * ry * ry - denominator)
    factor = math.sqrt(numerator / denominator) * (-1 if large == sweep else 1)
    cxp, cyp = factor * rx * yp / ry, -factor * ry * xp / rx
    center = (
        c * cxp - s * cyp + (start[0] + end[0]) / 2,
        s * cxp + c * cyp + (start[1] + end[1]) / 2,
    )
    theta = math.atan2((yp - cyp) / ry, (xp - cxp) / rx)
    last = math.atan2((-yp - cyp) / ry, (-xp - cxp) / rx)
    delta = (last - theta) % (2 * math.pi)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    center_t = transform_point(center, mat)
    a, b, cc, d, _, _ = mat
    u = (a * rx * c + cc * rx * s, b * rx * c + d * rx * s)
    v = (-a * ry * s + cc * ry * c, -b * ry * s + d * ry * c)
    points = [p0, p1]
    for base in (math.atan2(v[0], u[0]), math.atan2(v[1], u[1])):
        for t in (base, base + math.pi):
            distance = ((t - theta) % (2 * math.pi)) if sweep else ((theta - t) % (2 * math.pi))
            if distance <= abs(delta) + 1e-12:
                points.append(
                    (
                        center_t[0] + u[0] * math.cos(t) + v[0] * math.sin(t),
                        center_t[1] + u[1] * math.cos(t) + v[1] * math.sin(t),
                    )
                )
    return points


def path_bounds(text, mat=IDENTITY):
    """Exact geometric bounds after the affine transform, excluding stroke."""
    points = []
    for segment in path_segments(text):
        kind = segment[0]
        if kind == "L":
            points.extend(transform_point(p, mat) for p in segment[1:])
        elif kind in ("Q", "C"):
            points.extend(_curve_points([transform_point(p, mat) for p in segment[1:]]))
        else:
            points.extend(_arc_points(*segment[1:], mat))
    if not points:
        return None
    return (
        min(p[0] for p in points),
        min(p[1] for p in points),
        max(p[0] for p in points),
        max(p[1] for p in points),
    )


def union_bounds(bounds):
    bounds = [box for box in bounds if box is not None]
    if not bounds:
        return None
    return (
        min(b[0] for b in bounds),
        min(b[1] for b in bounds),
        max(b[2] for b in bounds),
        max(b[3] for b in bounds),
    )


def use_position(el):
    try:
        x, y = float(el.get("x", 0)), float(el.get("y", 0))
    except ValueError as exc:
        raise ValueError("invalid SVG use position") from exc
    if not math.isfinite(x) or not math.isfinite(y):
        raise ValueError("non-finite SVG use position")
    return (1, 0, 0, 1, x, y)


def min_scale(mat):
    """Smallest affine scale, including anisotropic scales and rotations."""
    a, b, c, d, _, _ = mat
    largest = math.sqrt(
        max(
            0,
            (a * a + b * b + c * c + d * d + math.hypot(a * a + b * b - c * c - d * d, 2 * (a * c + b * d))) / 2,
        )
    )
    return abs(a * d - b * c) / largest if largest else 0.0


def style_properties(el):
    properties = dict(el.attrib)
    for item in el.get("style", "").split(";"):
        if not item.strip():
            continue
        name, separator, value = item.partition(":")
        if not separator:
            raise ValueError(f"invalid SVG style declaration {item!r}")
        properties[name.strip().lower()] = re.sub(r"\s*!important\s*$", "", value.strip(), flags=re.IGNORECASE)
    return properties


def zero_opacity(value):
    value = value.strip()
    value = value.removesuffix("%")
    try:
        number = float(value)
    except ValueError as exc:
        raise ValueError(f"invalid SVG opacity {value!r}") from exc
    if not math.isfinite(number):
        raise ValueError("non-finite SVG opacity")
    return number <= 0


def visibility_state(el, inherited, hidden):
    properties = style_properties(el)
    state = dict(inherited)
    for name in ("fill", "stroke", "fill-opacity", "stroke-opacity"):
        value = properties.get(name)
        if value is not None and value.strip().lower() != "inherit":
            state[name] = value.strip().lower()
    reasons = list(hidden)
    if properties.get("display", "").strip().lower() == "none":
        reasons.append("display none")
    if properties.get("visibility", "").strip().lower() in ("hidden", "collapse"):
        reasons.append("visibility hidden")
    if (
        "opacity" in properties
        and properties["opacity"].strip().lower() != "inherit"
        and zero_opacity(properties["opacity"])
    ):
        reasons.append("opacity 0")
    return state, reasons


def unpainted(state):
    def invisible(name):
        return state[name] in ("none", "transparent") or zero_opacity(state[name + "-opacity"])

    return invisible("fill") and invisible("stroke")


def parse_svg_page(svg_text):
    root = ET.fromstring(svg_text)
    if root.tag != SVG + "svg":
        raise ValueError("expected an SVG document")
    if next(root.iter(SVG + "style"), None) is not None:
        raise ValueError("SVG stylesheets are not supported by the visibility check")
    scan = Scanner(root.get("viewBox", ""))
    viewbox = []
    while True:
        scan.skip()
        if scan.pos == len(scan.text):
            break
        viewbox.append(scan.number())
    if len(viewbox) != 4 or viewbox[2] <= 0 or viewbox[3] <= 0:
        raise ValueError("invalid or missing SVG viewBox")
    vx, vy, vw, vh = viewbox
    limits = (vx, vy, vx + vw, vy + vh)

    definitions = {}
    parents = {child: parent for parent in root.iter() for child in parent}
    for defs in root.iter(SVG + "defs"):
        for el in defs.iter():
            if "id" in el.attrib:
                if el.attrib["id"] in definitions:
                    raise ValueError(f"duplicate SVG definition {el.attrib['id']}")
                definitions[el.attrib["id"]] = el

    def href(el):
        value = el.get("href") or el.get(XLINK + "href") or ""
        if not value.startswith("#") or value[1:] not in definitions:
            raise ValueError(f"unknown SVG use reference {value!r}")
        return value[1:]

    def ref_bounds(el, parent, seen):
        mat = compose(parent, parse_transform(el.get("transform")))
        if el.tag == SVG + "path":
            return path_bounds(el.get("d", ""), mat)
        if el.tag == SVG + "use":
            name = href(el)
            if name in seen:
                raise ValueError(f"cyclic SVG use reference {name!r}")
            placed = compose(mat, use_position(el))
            return ref_bounds(definitions[name], placed, seen | {name})
        return union_bounds(ref_bounds(child, mat, seen) for child in el)

    def ref_scales(el, parent, seen):
        mat = compose(parent, parse_transform(el.get("transform")))
        if el.tag == SVG + "path":
            return [min_scale(mat)] if path_bounds(el.get("d", "")) is not None else []
        if el.tag == SVG + "use":
            name = href(el)
            if name in seen:
                raise ValueError(f"cyclic SVG use reference {name!r}")
            return ref_scales(definitions[name], compose(mat, use_position(el)), seen | {name})
        return [scale for child in el for scale in ref_scales(child, mat, seen)]

    def definition_state(name, state, hidden):
        ancestors = []
        node = definitions[name]
        while node in parents and parents[node] is not root:
            node = parents[node]
            ancestors.append(node)
        for ancestor in reversed(ancestors):
            state, hidden = visibility_state(ancestor, state, hidden)
        return state, hidden

    glyph_local = {}
    paths = []
    glyphs = []
    text_runs = 0
    visibility_violations = []

    def check_paint(label, state, hidden):
        if hidden:
            visibility_violations.append(f"{label} hidden by {', '.join(hidden)}")
        elif unpainted(state):
            visibility_violations.append(f"{label} has no visible fill or stroke")

    def check_reference_paint(el, state, hidden, seen, label):
        state, hidden = visibility_state(el, state, hidden)
        if el.tag == SVG + "path" and path_bounds(el.get("d", "")) is not None:
            check_paint(label, state, hidden)
        elif el.tag == SVG + "use":
            name = href(el)
            if name in seen:
                raise ValueError(f"cyclic SVG use reference {name!r}")
            state, hidden = definition_state(name, state, hidden)
            check_reference_paint(definitions[name], state, hidden, seen | {name}, label)
        for child in el:
            check_reference_paint(child, state, hidden, seen, label)

    def walk(el, parent, inherited, hidden):
        nonlocal text_runs
        if el.tag in (SVG + "defs", SVG + "clipPath", SVG + "mask"):
            return
        mat = compose(parent, parse_transform(el.get("transform")))
        state, hidden = visibility_state(el, inherited, hidden)
        if el.tag == SVG + "path":
            box = path_bounds(el.get("d", ""), mat)
            paths.append(box)
            if box is not None:
                check_paint("path", state, hidden)
        elif el.tag == SVG + "use":
            name = href(el)
            if name not in glyph_local:
                glyph_local[name] = ref_bounds(definitions[name], IDENTITY, {name})
            local = glyph_local[name]
            notehead = False
            if local:
                w, h = local[2] - local[0], local[3] - local[1]
                notehead = 4.0 <= w <= 9.0 and 2.5 <= h <= 7.5 and local[1] < 0 < local[3]
                ref_state, ref_hidden = definition_state(name, state, hidden)
                check_reference_paint(definitions[name], ref_state, ref_hidden, {name}, f"glyph {name}")
            placed = compose(mat, use_position(el))
            scales = ref_scales(definitions[name], placed, {name}) if local else []
            glyphs.append(
                (
                    name,
                    ref_bounds(definitions[name], placed, {name}),
                    notehead,
                    min(scales) if scales else None,
                )
            )
        if el.tag == SVG + "g" and len(el.findall(SVG + "use")) > 1:
            text_runs += 1
        for child in el:
            walk(child, mat, state, hidden)

    walk(
        root,
        IDENTITY,
        {"fill": "black", "stroke": "none", "fill-opacity": "1", "stroke-opacity": "1"},
        [],
    )
    violations = []
    for label, box in [("path", box) for box in paths] + [(f"glyph {name}", box) for name, box, _, _ in glyphs]:
        if box and (
            box[0] < limits[0] - VIEWBOX_EPSILON
            or box[1] < limits[1] - VIEWBOX_EPSILON
            or box[2] > limits[2] + VIEWBOX_EPSILON
            or box[3] > limits[3] + VIEWBOX_EPSILON
        ):
            violations.append(f"{label} bounds {tuple(round(v, 2) for v in box)} outside viewBox {tuple(viewbox)}")

    overlap_pairs = 0
    overlap_area = 0.0
    boxes = [box for _, box, _, _ in glyphs if box]
    for i, first in enumerate(boxes):
        for second in boxes[i + 1 :]:
            width = min(first[2], second[2]) - max(first[0], second[0])
            height = min(first[3], second[3]) - max(first[1], second[1])
            if width > OVERLAP_EPSILON and height > OVERLAP_EPSILON:
                overlap_pairs += 1
                overlap_area += width * height
    scales = {}
    sizes = {}
    for _, box, notehead, scale in glyphs:
        if scale is not None:
            kind = "notehead" if notehead else "other"
            scales[kind] = min(scale, scales.get(kind, scale))
            if box:
                sizes.setdefault(kind, []).append((box[2] - box[0], box[3] - box[1]))
    medians = {
        kind: [statistics.median(size[axis] for size in values) for axis in (0, 1)] for kind, values in sizes.items()
    }
    return PageStats(
        viewbox,
        len(paths),
        text_runs,
        sum(g[2] for g in glyphs),
        len(glyphs),
        overlap_pairs,
        round(overlap_area, 6),
        scales,
        medians,
        violations,
        visibility_violations,
    )


def compare_stat(actual, reference, delta):
    if reference == 0:
        return None, None if actual == 0 else f"actual {actual}, reference 0"
    ratio = actual / reference
    return ratio, None if abs(ratio - 1) <= delta + 1e-12 else f"ratio {ratio:.4f} outside ±{delta:.0%}"


def page_issues(actual, reference, count_exception=None):
    issues = list(actual.bbox_violations) + list(actual.visibility_violations)
    if len(reference["viewbox"]) != 4 or any(
        abs(a - b) > VIEWBOX_EPSILON for a, b in zip(actual.viewbox, reference["viewbox"])
    ):
        issues.append(f"viewBox {actual.viewbox} differs from reference {reference['viewbox']}")
    for name in ("paths", "text_runs", "noteheads", "total_uses"):
        expected = (count_exception or {}).get(name, reference[name])
        if getattr(actual, name) != expected:
            issues.append(f"{name}: actual {getattr(actual, name)}, expected {expected}")
    for name in ("overlap_pairs", "overlap_area"):
        _, error = compare_stat(getattr(actual, name), reference[name], OVERLAP_DELTA)
        if error:
            issues.append(f"{name}: {error}")
    for kind, scale in reference["glyph_min_scales"].items():
        actual_scale = actual.glyph_min_scales.get(kind, 0)
        if actual_scale < MIN_GLYPH_SCALE_RATIO * scale:
            issues.append(
                f"{kind} glyph minimum scale {actual_scale:.6g} below "
                f"{MIN_GLYPH_SCALE_RATIO:g} of reference {scale:.6g}"
            )
    for kind, size in reference["glyph_median_sizes"].items():
        actual_size = actual.glyph_median_sizes.get(kind, (0, 0))
        if any(a < MIN_GLYPH_SCALE_RATIO * b for a, b in zip(actual_size, size)):
            issues.append(f"{kind} glyph median size {actual_size} below {MIN_GLYPH_SCALE_RATIO:g} of reference {size}")
    return issues


def count_exceptions(payload):
    result = {}
    for entry in payload.get("count_exceptions", []):
        name, digest, counts, reason = (
            entry.get("page"),
            entry.get("svg_sha256"),
            entry.get("counts"),
            entry.get("reason"),
        )
        if (
            name not in payload["pages"]
            or name in result
            or not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
            or not isinstance(reason, str)
            or not reason.strip()
            or not isinstance(counts, dict)
            or not counts
            or any(
                k not in ("paths", "text_runs", "noteheads", "total_uses") or type(v) is not int or v < 0
                for k, v in counts.items()
            )
        ):
            raise ValueError(f"invalid count exception for {name!r}")
        result[name] = entry
    return result


def png_exceptions(payload):
    """Validate page- and platform-specific pins for known snapshot drift."""
    result = {}
    for entry in payload.get("png_exceptions", []):
        name, platform, digest, issues, reason = (
            entry.get("page"),
            entry.get("platform"),
            entry.get("png_sha256"),
            entry.get("issues"),
            entry.get("reason"),
        )
        svg = str(Path(name).with_suffix(".svg").as_posix()) if isinstance(name, str) else None
        key = (platform, name)
        if (
            svg not in payload["pages"]
            or payload["pages"][svg].get("png") is None
            or platform not in ("windows", "linux")
            or key in result
            or not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
            or not isinstance(reason, str)
            or not reason.strip()
            or not isinstance(issues, list)
            or not issues
            or any(not isinstance(issue, str) or not issue.strip() for issue in issues)
        ):
            raise ValueError(f"invalid PNG exception for {name!r}")
        result[key] = entry
    return result


def accepted_svg_counts(name, digest):
    """Return independently reviewed counts, never counts from the tested SVG."""
    platform = "windows" if sys.platform.startswith("win") else "linux"
    path = Path(__file__).resolve().parents[1] / "data/golden/page_count_pins.json"
    pins = json.loads(path.read_text(encoding="utf-8"))
    entry = pins.get(platform, {}).get(name)
    return entry["counts"] if entry and entry["svg_sha256"] == digest else None


def current_png_oracle(path, name, platform):
    """Match decoded pixels to the indexed current descriptor."""
    from support import golden as golden

    fixture, output = name.split("/", 1)
    root = ROOT / "vendor/engrave/tests/data/golden" / platform
    index = json.loads((root / "index.json").read_text(encoding="utf-8"))
    digest = index.get(fixture, {}).get(output + ".json")
    baseline = root / fixture / (output + ".json")
    if not digest or not baseline.is_file() or golden.sha256(baseline.read_bytes()) != digest:
        return False
    descriptor = (
        json.dumps(
            golden.describe_png(path),
            indent=1,
            sort_keys=True,
            ensure_ascii=False,
            allow_nan=False,
        )
        + "\n"
    )
    return hashlib.sha256(descriptor.encode()).hexdigest() == digest


def png_stats(path):
    """Measure visible ink after compositing RGBA over white, in page coordinates."""
    import numpy as np
    from PIL import Image

    with Image.open(path) as image:
        rgba = np.asarray(image.convert("RGBA"), dtype=np.uint16)
    height, width, _ = rgba.shape
    if not width or not height:
        raise ValueError("PNG has empty dimensions")
    alpha = rgba[:, :, 3, None]
    rgb = (rgba[:, :, :3] * alpha + 255 * (255 - alpha) + 127) // 255
    ink = np.min(rgb, axis=2) < INK_THRESHOLD
    count = int(ink.sum())
    ys, xs = np.nonzero(ink)
    bbox = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1] if count else None
    grid = []
    for row in range(GRID_SIZE):
        line = []
        for col in range(GRID_SIZE):
            cell = ink[
                row * height // GRID_SIZE : (row + 1) * height // GRID_SIZE,
                col * width // GRID_SIZE : (col + 1) * width // GRID_SIZE,
            ]
            if not cell.size:
                raise ValueError("PNG is smaller than the ink grid")
            line.append(round(float(cell.mean()), 10))
        grid.append(line)
    return {
        "size": [width, height],
        "ink_share": count / (width * height),
        "bbox": bbox,
        "grid_density": grid,
    }


def png_metrics(actual, reference):
    width, height = reference["size"]
    if reference["ink_share"] <= 0 or reference["bbox"] is None:
        raise ValueError("PNG reference has no ink")
    edge_delta = (
        max(abs(a - b) / dim for a, b, dim in zip(actual["bbox"], reference["bbox"], (width, height, width, height)))
        if actual["bbox"] is not None
        else None
    )
    if (
        len(actual["grid_density"]) != GRID_SIZE
        or len(reference["grid_density"]) != GRID_SIZE
        or any(len(row) != GRID_SIZE for row in actual["grid_density"] + reference["grid_density"])
    ):
        raise ValueError("invalid PNG ink grid")
    weighted_difference = 0.0
    for row in range(GRID_SIZE):
        for col in range(GRID_SIZE):
            cell_area = ((row + 1) * height // GRID_SIZE - row * height // GRID_SIZE) * (
                (col + 1) * width // GRID_SIZE - col * width // GRID_SIZE
            )
            weighted_difference += (
                abs(actual["grid_density"][row][col] - reference["grid_density"][row][col]) * cell_area
            )
    return (
        actual["ink_share"] / reference["ink_share"],
        edge_delta,
        weighted_difference / (reference["ink_share"] * width * height),
    )


def png_issues(actual, reference):
    issues = []
    if actual["size"] != reference["size"]:
        # A dimension mismatch must not hide an independent change in the ink.
        issues.append(f"PNG size {actual['size']} differs from reference {reference['size']}")
    ratio, edge_delta, grid_l1 = png_metrics(actual, reference)
    if abs(ratio - 1) > INK_RATIO_DELTA + 1e-12:
        issues.append(f"PNG ink share ratio {ratio:.4f} outside ±{INK_RATIO_DELTA:.0%}")
    if edge_delta is None:
        issues.append("PNG has no ink bounding box")
    elif edge_delta > BBOX_EDGE_DELTA:
        issues.append(f"PNG bbox edge delta {edge_delta:.4f} exceeds {BBOX_EDGE_DELTA:.4f}")
    if grid_l1 > GRID_L1_LIMIT:
        issues.append(f"PNG grid L1 {grid_l1:.4f} exceeds {GRID_L1_LIMIT:.4f}")
    return issues


async def capture_pages(binary, resources, soundfont, output):
    """Capture the golden harness's SVGs and master page-0 PNGs through MCP."""
    from support.golden import (
        GENERATE_EXCERPTS,
        ClientSession,
        StdioServerParameters,
        call_tool,
        export,
        fixture_files,
        generate_excerpts,
        normalize_svg,
        open_fixture,
        required,
        stdio_client,
        write_text,
    )

    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f"capture directory is not empty: {output}")
    output = output.resolve()
    params = StdioServerParameters(
        command=str(binary.resolve()),
        args=[
            "--resources",
            str(resources.resolve()),
            "--soundfont",
            str(soundfont.resolve()),
        ],
        cwd=str(output),
        env=dict(os.environ),
    )
    with (output / "server-stderr.log").open("w", encoding="utf-8") as log:
        async with stdio_client(params, errlog=log) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                for source in fixture_files():
                    folder = output / source.name
                    folder.mkdir()
                    payload = await open_fixture(session, source)
                    identity = {"document": payload["document"]}
                    try:
                        outline = await required(session, "score_outline", identity)
                        pages = outline["outline"]["pages"]
                        for index in range(int(pages)):
                            path = folder / f"page-{index}.svg"
                            error = await export(session, identity, "svg", path, page=index)
                            if error:
                                raise ValueError(f"{source.name}: SVG page {index}: {error}")
                            write_text(path, normalize_svg(path.read_text(encoding="utf-8")))
                        error = await export(session, identity, "png", folder / "page-0.png", page=0)
                        if error:
                            raise ValueError(f"{source.name}: PNG page 0: {error}")
                        if source.name in GENERATE_EXCERPTS:
                            await generate_excerpts(session, identity)
                        excerpts = (await required(session, "score_outline", identity))["outline"]["excerpts"]
                        for index, excerpt in enumerate(excerpts):
                            path = folder / f"excerpt-{index}" / "page-0.svg"
                            path.parent.mkdir()
                            error = await export(
                                session,
                                identity,
                                "svg",
                                path,
                                excerpt=excerpt["id"],
                                page=0,
                            )
                            if error:
                                raise ValueError(f"{source.name}: excerpt {index}: {error}")
                            write_text(path, normalize_svg(path.read_text(encoding="utf-8")))
                        print(
                            f"captured {source.name}: {pages} master SVG page(s), PNG page 0, "
                            f"{len(excerpts)} excerpt(s)",
                            flush=True,
                        )
                    finally:
                        await call_tool(session, "score_close", identity)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--golden",
        type=Path,
        default=ROOT / "vendor/engrave/tests/data/golden/windows",
        help="directory of already captured SVG and PNG pages to check",
    )
    parser.add_argument(
        "--reference",
        type=Path,
        default=ROOT / "vendor/engrave/tests/data/golden/page_reference.json",
        help="checked-in 4.7.6 statistics",
    )
    parser.add_argument(
        "--capture-only",
        action="store_true",
        help="capture exports without comparing them",
    )
    parser.add_argument("--binary", type=Path, help="MCP host used for page capture")
    parser.add_argument("--resources", type=Path, help="host resources for page capture")
    parser.add_argument("--soundfont", type=Path, help="host soundfont for page capture")
    parser.add_argument("--output", type=Path, help="directory for captured pages")
    args = parser.parse_args()
    if args.capture_only:
        if not all((args.binary, args.resources, args.soundfont, args.output)):
            parser.error("--capture-only requires --binary, --resources, --soundfont and --output")
        try:
            asyncio.run(
                asyncio.wait_for(
                    capture_pages(args.binary, args.resources, args.soundfont, args.output),
                    timeout=1800,
                )
            )
        except (ValueError, OSError, RuntimeError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        return 0
    if any((args.binary, args.resources, args.soundfont, args.output)):
        if not all((args.binary, args.resources, args.soundfont, args.output)):
            parser.error("page capture requires --binary, --resources, --soundfont and --output")
        try:
            args.output.mkdir(parents=True, exist_ok=True)
            args.golden = Path(tempfile.mkdtemp(prefix="capture-", dir=args.output))
            asyncio.run(
                asyncio.wait_for(
                    capture_pages(args.binary, args.resources, args.soundfont, args.golden),
                    timeout=1800,
                )
            )
        except (ValueError, OSError, RuntimeError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"Captured pages: {args.golden}")
    if not args.golden.is_dir() or not args.reference.is_file():
        print("error: missing golden directory or reference file", file=sys.stderr)
        return 2
    payload = json.loads(args.reference.read_text(encoding="utf-8"))
    if payload.get("version") != 3 or not isinstance(payload.get("pages"), dict):
        print("error: invalid page reference format", file=sys.stderr)
        return 2
    pages = payload["pages"]
    if any(not isinstance(entry, dict) or "png" not in entry for entry in pages.values()):
        print(
            "error: each SVG reference must declare its PNG reference or null",
            file=sys.stderr,
        )
        return 2
    try:
        exceptions = count_exceptions(payload)
        png_pins = png_exceptions(payload)
    except (AttributeError, KeyError, TypeError, ValueError) as exc:
        print(f"error: invalid page exceptions: {exc}", file=sys.stderr)
        return 2
    files = sorted(args.golden.rglob("*.svg"))
    if not files:
        print(f"error: no SVG pages in {args.golden}", file=sys.stderr)
        return 2
    print(f"Rendered-page spot check: {len(files)} SVG pages against {args.reference.name}")
    failed = 0
    for path in files:
        name = path.relative_to(args.golden).as_posix()
        reference = pages.get(name)
        if not reference:
            print(f"{name}: FAIL: missing reference")
            failed += 1
            continue
        try:
            data = path.read_bytes()
            actual = parse_svg_page(data.decode("utf-8"))
            exception = exceptions.get(name)
            issues = []
            allowed_counts = None
            if exception:
                digest = hashlib.sha256(data).hexdigest()
                if digest != exception["svg_sha256"]:
                    if counts := accepted_svg_counts(name, digest):
                        allowed_counts = counts
                    else:
                        issues.append("count exception SVG SHA-256 does not match")
                elif any(
                    value == reference["stats"][key] or getattr(actual, key) != value
                    for key, value in exception["counts"].items()
                ):
                    issues.append("stale count exception or exception count does not match")
                else:
                    allowed_counts = exception["counts"]
            issues.extend(page_issues(actual, reference["stats"], allowed_counts))
        except (ValueError, KeyError, ET.ParseError) as exc:
            issues = [f"invalid page or reference: {exc}"]
        print(f"{name}: {'FAIL' if issues else 'PASS'}")
        if issues:
            failed += 1
            for issue in issues[:5]:
                print(f"    {issue}")
            if len(issues) > 5:
                print(f"    ... and {len(issues) - 5} more")
    missing = sorted(set(pages) - {p.relative_to(args.golden).as_posix() for p in files})
    for name in missing:
        print(f"{name}: FAIL: reference page missing from golden directory")
    failed += len(missing)
    expected_png = {
        str(Path(name).with_suffix(".png").as_posix()): entry["png"]
        for name, entry in pages.items()
        if entry["png"] is not None
    }
    svg_only = [name for name, entry in pages.items() if entry["png"] is None]
    actual_png = {path.relative_to(args.golden).as_posix(): path for path in args.golden.rglob("*.png")}
    for name in sorted(set(expected_png) | set(actual_png)):
        issues = []
        detail = ""
        if name not in expected_png:
            issues.append("PNG has no reference")
        elif name not in actual_png:
            issues.append("reference PNG missing from capture")
        else:
            try:
                actual = png_stats(actual_png[name])
                reference = expected_png[name]["stats"]
                issues = png_issues(actual, reference)
                platform = "windows" if sys.platform.startswith("win") else "linux"
                pin = png_pins.get((platform, name))
                if pin and not issues and current_png_oracle(actual_png[name], name, platform):
                    detail = " (exact direct-render oracle restored)"
                elif pin:
                    digest = hashlib.sha256(actual_png[name].read_bytes()).hexdigest()
                    if digest != pin["png_sha256"]:
                        issues.append("precision exception PNG SHA-256 does not match")
                    elif issues != pin["issues"]:
                        issues.append("precision exception diagnostics do not match")
                    else:
                        detail = " (exact snapshot precision pin; " + "; ".join(issues) + ")"
                        issues = []
                if not detail and actual["size"] == reference["size"]:
                    ratio, edge, grid = png_metrics(actual, reference)
                    detail = (
                        f" (ink {reference['ink_share']:.6f}->{actual['ink_share']:.6f}, "
                        f"ratio {ratio:.4f}, bbox edge {edge:.4f}, grid L1 {grid:.4f})"
                        if edge is not None
                        else " (no ink bbox)"
                    )
            except (ValueError, KeyError, OSError, TypeError) as exc:
                issues = [f"invalid PNG or reference: {exc}"]
        print(f"{name}: {'FAIL' if issues else 'PASS'}{detail}")
        if issues:
            failed += 1
            for issue in issues:
                print(f"    {issue}")
    for name in svg_only:
        print(f"{name}: SVG only (golden harness exports PNG for master page 0 only)")
    print(
        f"Total: {len(files)} SVG pages, {len(actual_png)} PNG pages, "
        f"{len(svg_only)} SVG-only pages, {failed} failure(s)"
    )
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
