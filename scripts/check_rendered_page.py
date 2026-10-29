"""Compare rendered SVG page geometry with immutable 4.7.6 statistics.

This is a geometry/content spot check, not a visual proof of legibility. Path
bounds are centerline/fill geometry (stroke width and clipping are not modeled).
Glyph overlap is measured between transformed outline bounding boxes.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SVG = "{http://www.w3.org/2000/svg}"
XLINK = "{http://www.w3.org/1999/xlink}"
IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
NUMBER = re.compile(r"[-+]?(?:(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)")

# The existing count check allowed 5% drift. The 10% overlap margins allow small
# glyph-box changes while detecting substantial added collisions. Zero reference
# values must still match exactly; no artificial ratio is assigned to 0/0.
COUNT_DELTA = 0.05
OVERLAP_DELTA = 0.10
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
    bbox_violations: list[str] = field(default_factory=list)


def compose(a, b):
    """Compose SVG affine matrices as a * b, so b acts on a point first."""
    a0, a1, a2, a3, a4, a5 = a
    b0, b1, b2, b3, b4, b5 = b
    return (a0*b0 + a2*b1, a1*b0 + a3*b1,
            a0*b2 + a2*b3, a1*b2 + a3*b3,
            a0*b4 + a2*b5 + a4, a1*b4 + a3*b5 + a5)


def transform_point(point, mat):
    x, y = point
    a, b, c, d, e, f = mat
    return a*x + c*y + e, b*x + d*y + f


class Scanner:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def skip(self):
        while self.pos < len(self.text) and (self.text[self.pos].isspace()
                                              or self.text[self.pos] == ","):
            self.pos += 1

    def number(self):
        self.skip()
        match = NUMBER.match(self.text, self.pos)
        if not match:
            raise ValueError(f"expected SVG number at {self.text[self.pos:self.pos+20]!r}")
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
                matrix = compose(compose((1, 0, 0, 1, cx, cy), matrix),
                                 (1, 0, 0, 1, -cx, -cy))
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
            raise ValueError(f"expected SVG path command at {text[scan.pos:scan.pos+20]!r}")

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
            c1 = (2*current[0]-control[0], 2*current[1]-control[1]) if previous in ("C", "S") else current
            c2, end = point(relative), point(relative)
            segments.append(("C", current, c1, c2, end))
            current, control = end, c2
        elif kind == "Q":
            c1, end = point(relative), point(relative)
            segments.append(("Q", current, c1, end))
            current, control = end, c1
        elif kind == "T":
            c1 = (2*current[0]-control[0], 2*current[1]-control[1]) if previous in ("Q", "T") else current
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
        return [] if abs(b) < 1e-14 else [-c/b]
    disc = b*b - 4*a*c
    if disc < 0:
        return []
    root = math.sqrt(disc)
    return [(-b + root)/(2*a), (-b - root)/(2*a)]


def _curve_points(points):
    """Return endpoints and the extrema of a transformed quadratic/cubic."""
    found = [points[0], points[-1]]
    ts = []
    for axis in (0, 1):
        p = [value[axis] for value in points]
        if len(p) == 3:
            denominator = p[0] - 2*p[1] + p[2]
            if abs(denominator) > 1e-14:
                ts.append((p[0] - p[1]) / denominator)
        else:
            a = -p[0] + 3*p[1] - 3*p[2] + p[3]
            b = 3*p[0] - 6*p[1] + 3*p[2]
            c = 3*(p[1] - p[0])
            ts.extend(_quadratic_roots(3*a, 2*b, c))
    for t in ts:
        if 0 < t < 1:
            work = list(points)
            while len(work) > 1:
                work = [((1-t)*a[0] + t*b[0], (1-t)*a[1] + t*b[1])
                        for a, b in zip(work, work[1:])]
            found.append(work[0])
    return found


def _arc_points(start, end, rx, ry, rotation, large, sweep, mat):
    """SVG endpoint-form elliptical arc, including radii correction and extrema."""
    p0, p1 = transform_point(start, mat), transform_point(end, mat)
    if start == end or rx == 0 or ry == 0:
        return [p0, p1]
    phi = math.radians(rotation % 360)
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (start[0]-end[0])/2, (start[1]-end[1])/2
    xp, yp = c*dx + s*dy, -s*dx + c*dy
    scale = xp*xp/(rx*rx) + yp*yp/(ry*ry)
    if scale > 1:
        rx, ry = rx*math.sqrt(scale), ry*math.sqrt(scale)
    denominator = rx*rx*yp*yp + ry*ry*xp*xp
    numerator = max(0.0, rx*rx*ry*ry - denominator)
    factor = math.sqrt(numerator/denominator) * (-1 if large == sweep else 1)
    cxp, cyp = factor*rx*yp/ry, -factor*ry*xp/rx
    center = (c*cxp - s*cyp + (start[0]+end[0])/2,
              s*cxp + c*cyp + (start[1]+end[1])/2)
    theta = math.atan2((yp-cyp)/ry, (xp-cxp)/rx)
    last = math.atan2((-yp-cyp)/ry, (-xp-cxp)/rx)
    delta = (last-theta) % (2*math.pi)
    if not sweep and delta > 0:
        delta -= 2*math.pi
    center_t = transform_point(center, mat)
    a, b, cc, d, _, _ = mat
    u = (a*rx*c + cc*rx*s, b*rx*c + d*rx*s)
    v = (-a*ry*s + cc*ry*c, -b*ry*s + d*ry*c)
    points = [p0, p1]
    for base in (math.atan2(v[0], u[0]), math.atan2(v[1], u[1])):
        for t in (base, base + math.pi):
            distance = ((t-theta) % (2*math.pi)) if sweep else ((theta-t) % (2*math.pi))
            if distance <= abs(delta) + 1e-12:
                points.append((center_t[0] + u[0]*math.cos(t) + v[0]*math.sin(t),
                               center_t[1] + u[1]*math.cos(t) + v[1]*math.sin(t)))
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
    return (min(p[0] for p in points), min(p[1] for p in points),
            max(p[0] for p in points), max(p[1] for p in points))


def union_bounds(bounds):
    bounds = [box for box in bounds if box is not None]
    if not bounds:
        return None
    return (min(b[0] for b in bounds), min(b[1] for b in bounds),
            max(b[2] for b in bounds), max(b[3] for b in bounds))


def use_position(el):
    try:
        x, y = float(el.get("x", 0)), float(el.get("y", 0))
    except ValueError as exc:
        raise ValueError("invalid SVG use position") from exc
    if not math.isfinite(x) or not math.isfinite(y):
        raise ValueError("non-finite SVG use position")
    return (1, 0, 0, 1, x, y)


def parse_svg_page(svg_text):
    root = ET.fromstring(svg_text)
    if root.tag != SVG + "svg":
        raise ValueError("expected an SVG document")
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
    limits = (vx, vy, vx+vw, vy+vh)

    definitions = {}
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

    glyph_local = {}
    paths = []
    glyphs = []
    text_runs = 0

    def walk(el, parent):
        nonlocal text_runs
        if el.tag in (SVG + "defs", SVG + "clipPath", SVG + "mask"):
            return
        mat = compose(parent, parse_transform(el.get("transform")))
        if el.tag == SVG + "path":
            paths.append(path_bounds(el.get("d", ""), mat))
        elif el.tag == SVG + "use":
            name = href(el)
            if name not in glyph_local:
                glyph_local[name] = ref_bounds(definitions[name], IDENTITY, {name})
            local = glyph_local[name]
            notehead = False
            if local:
                w, h = local[2]-local[0], local[3]-local[1]
                notehead = 4.0 <= w <= 9.0 and 2.5 <= h <= 7.5 and local[1] < 0 < local[3]
            placed = compose(mat, use_position(el))
            glyphs.append((name, ref_bounds(definitions[name], placed, {name}), notehead))
        if el.tag == SVG + "g" and len(el.findall(SVG + "use")) > 1:
            text_runs += 1
        for child in el:
            walk(child, mat)

    walk(root, IDENTITY)
    violations = []
    for label, box in ([("path", box) for box in paths] +
                       [(f"glyph {name}", box) for name, box, _ in glyphs]):
        if box and (box[0] < limits[0]-VIEWBOX_EPSILON or box[1] < limits[1]-VIEWBOX_EPSILON
                    or box[2] > limits[2]+VIEWBOX_EPSILON or box[3] > limits[3]+VIEWBOX_EPSILON):
            violations.append(f"{label} bounds {tuple(round(v, 2) for v in box)} outside viewBox {tuple(viewbox)}")

    overlap_pairs = 0
    overlap_area = 0.0
    boxes = [box for _, box, _ in glyphs if box]
    for i, first in enumerate(boxes):
        for second in boxes[i+1:]:
            width = min(first[2], second[2]) - max(first[0], second[0])
            height = min(first[3], second[3]) - max(first[1], second[1])
            if width > OVERLAP_EPSILON and height > OVERLAP_EPSILON:
                overlap_pairs += 1
                overlap_area += width*height
    return PageStats(viewbox, len(paths), text_runs, sum(g[2] for g in glyphs), len(glyphs),
                     overlap_pairs, round(overlap_area, 6), violations)


def compare_stat(actual, reference, delta):
    if reference == 0:
        return None, None if actual == 0 else f"actual {actual}, reference 0"
    ratio = actual / reference
    return ratio, None if abs(ratio - 1) <= delta + 1e-12 else f"ratio {ratio:.4f} outside ±{delta:.0%}"


def page_issues(actual, reference):
    issues = list(actual.bbox_violations)
    if len(reference["viewbox"]) != 4 or any(abs(a-b) > VIEWBOX_EPSILON for a, b in zip(actual.viewbox, reference["viewbox"])):
        issues.append(f"viewBox {actual.viewbox} differs from reference {reference['viewbox']}")
    for name in ("paths", "text_runs", "noteheads", "total_uses", "overlap_pairs", "overlap_area"):
        tolerance = OVERLAP_DELTA if name.startswith("overlap_") else COUNT_DELTA
        _, error = compare_stat(getattr(actual, name), reference[name], tolerance)
        if error:
            issues.append(f"{name}: {error}")
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, default=ROOT / "tests/golden/windows",
                        help="directory of SVG pages to check")
    parser.add_argument("--reference", type=Path, default=ROOT / "tests/golden/page_reference.json",
                        help="checked-in 4.7.6 statistics")
    args = parser.parse_args()
    if not args.golden.is_dir() or not args.reference.is_file():
        print("error: missing golden directory or reference file", file=sys.stderr)
        return 2
    payload = json.loads(args.reference.read_text(encoding="utf-8"))
    if payload.get("version") != 2 or not isinstance(payload.get("pages"), dict):
        print("error: invalid page reference format", file=sys.stderr)
        return 2
    pages = payload["pages"]
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
            actual = parse_svg_page(path.read_text(encoding="utf-8"))
            issues = page_issues(actual, reference["stats"])
        except (ValueError, KeyError, ET.ParseError) as exc:
            issues = [f"invalid page or reference: {exc}"]
        print(f"{name}: {'FAIL' if issues else 'PASS'}")
        if issues:
            failed += 1
            for issue in issues[:5]:
                print(f"    {issue}")
            if len(issues) > 5:
                print(f"    ... and {len(issues)-5} more")
    missing = sorted(set(pages) - {p.relative_to(args.golden).as_posix() for p in files})
    for name in missing:
        print(f"{name}: FAIL: reference page missing from golden directory")
    failed += len(missing)
    print(f"Total: {len(files)} pages checked, {failed} failure(s)")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
