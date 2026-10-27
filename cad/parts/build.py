"""Build one part directory: STEP, STL and hidden-line views into <part>/build.

usage: python -m parts.build <part-dir>
"""
import importlib.util
import sys
from pathlib import Path

import pymupdf
from build123d import Compound, ExportSVG, LineType, export_step, export_stl

VIEWS = [("front", (-120, -160, 110)), ("back", (120, 160, 110))]   # a part sets its own


def load(part_dir: Path):
    spec = importlib.util.spec_from_file_location("part", part_dir / "part.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main(part_dir: Path) -> None:
    out = part_dir / "build"
    out.mkdir(exist_ok=True)
    mod = load(part_dir)
    built = mod.build()
    # One print per entry; a single-piece part returns one Part.
    prints = built if isinstance(built, dict) else {part_dir.name: built}
    for name, p in prints.items():
        bb = p.bounding_box()
        print(f"{name}: valid={p.is_valid} solids={len(p.solids())} volume={p.volume:.0f} mm3 "
              f"bbox {bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f} mm")
        export_step(p, str(out / f"{name}.step"))
        export_stl(p, str(out / f"{name}.stl"))
    # A part may describe the scene to draw: its prints placed with the bought parts around them.
    assembly = Compound(mod.scene(prints) if hasattr(mod, "scene") else list(prints.values()))
    pngs = []
    for view, eye in getattr(mod, "VIEWS", VIEWS):
        vis, hid = assembly.project_to_viewport(eye)
        e = ExportSVG(scale=5, line_weight=0.6)
        e.add_layer("hidden", line_color="#aaaaaa", line_type=LineType.ISO_DASH, line_weight=0.3)
        e.add_layer("visible", line_color="#141414", line_weight=0.7)
        e.add_shape(hid, layer="hidden")
        e.add_shape(vis, layer="visible")
        svg = out / f"{view}.svg"
        e.write(str(svg))
        pngs.append(pymupdf.open(str(svg))[0].get_pixmap(dpi=100))
    # One PNG with the views side by side, for the PR.
    w = sum(x.width for x in pngs) + 20 * (len(pngs) + 1)
    h = max(x.height for x in pngs) + 20
    doc = pymupdf.open()
    page = doc.new_page(width=w, height=h)
    x = 20
    for pix in pngs:
        page.insert_image(pymupdf.Rect(x, 10, x + pix.width, 10 + pix.height), pixmap=pix)
        x += pix.width + 20
    page.get_pixmap(dpi=72).save(str(out / "views.png"))
    print("wrote", sorted(f.name for f in out.iterdir()))


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
