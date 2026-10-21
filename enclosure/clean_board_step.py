"""Rewrite the board STEP so any CAD tool (e.g. Onshape) places every part correctly.

The vendor file (Open CASCADE 6.8 export) positions parts through nested assembly
transforms that some importers apply differently, scattering components. Here each
part is read with its full transform chain, the transform is BAKED into the
geometry, and the parts are written flat (no assembly transforms left to misread).
Parts are also moved into the case's local frame (origin = PCB centre, +Z = front,
Z0 = PCB top) so the board and the case STEPs line up when both sit at the origin.

Writes:
  out/E32R40T_board_full.step    every part (names + colours kept)
  out/E32R40T_board_simple.step  only parts that matter for enclosure design
                                 (drops tiny passives, pins, copper traces)
Run:  py -3 clean_board_step.py
"""
import os
import re
import sys

from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
from OCP.Quantity import Quantity_Color
from OCP.STEPCAFControl import STEPCAFControl_Reader, STEPCAFControl_Writer
from OCP.STEPControl import STEPControl_AsIs
from OCP.TCollection import TCollection_ExtendedString
from OCP.TDataStd import TDataStd_Name
from OCP.TDF import TDF_Label, TDF_LabelSequence
from OCP.TDocStd import TDocStd_Document
from OCP.TopLoc import TopLoc_Location
from OCP.gp import gp_Trsf, gp_Vec
from OCP.XCAFDoc import XCAFDoc_ColorGen, XCAFDoc_ColorSurf, XCAFDoc_DocumentTool

import build_case as b

SRC = b.BOARD_STEP
OUT = b.OUT


def read_parts(path):
    doc = TDocStd_Document(TCollection_ExtendedString("in"))
    r = STEPCAFControl_Reader()
    r.SetNameMode(True)
    r.SetColorMode(True)
    if r.ReadFile(path) != 1:
        sys.exit("cannot read " + path)
    r.Transfer(doc)
    st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
    ct = XCAFDoc_DocumentTool.ColorTool_s(doc.Main())

    def name(lab):
        n = TDataStd_Name()
        return n.Get().ToExtString() if lab.FindAttribute(TDataStd_Name.GetID_s(), n) else ""

    def color(lab):
        """Colour attached to this label's shape, if any (lookup is by shape)."""
        try:
            shp = st.GetShape_s(lab)
        except Exception:
            return None
        for kind in (XCAFDoc_ColorSurf, XCAFDoc_ColorGen):
            c = Quantity_Color()
            try:
                if ct.GetColor(shp, kind, c):
                    return c
            except Exception:
                return None
        return None

    parts = []

    def walk(lab, loc, inherited):
        col = color(lab) or inherited
        if st.IsReference_s(lab):
            ref = TDF_Label()
            st.GetReferredShape_s(lab, ref)
            walk(ref, loc.Multiplied(st.GetLocation_s(lab)), color(ref) or col)
            return
        if st.IsAssembly_s(lab):
            kids = TDF_LabelSequence()
            st.GetComponents_s(lab, kids)
            for i in range(1, kids.Length() + 1):
                walk(kids.Value(i), loc, col)
        else:
            parts.append((name(lab), st.GetShape_s(lab), loc, color(lab) or col))

    roots = TDF_LabelSequence()
    st.GetFreeShapes(roots)
    for i in range(1, roots.Length() + 1):
        walk(roots.Value(i), TopLoc_Location(), None)
    return parts


def bake(shape, loc):
    """Apply the accumulated transform + the case-frame offset to the geometry."""
    t = gp_Trsf()
    t.SetTranslation(gp_Vec(*b.STEP_TO_LOCAL))
    t.Multiply(loc.Transformation())
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def bbox(shape):
    bb = Bnd_Box()
    BRepBndLib.Add_s(shape, bb)
    return bb.Get()


def write(items, path):
    doc = TDocStd_Document(TCollection_ExtendedString("out"))
    st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
    ct = XCAFDoc_DocumentTool.ColorTool_s(doc.Main())
    used = {}
    for nm, shp, col in items:
        base = re.sub(r"[^A-Za-z0-9_.\-]+", "_", nm).strip("_") or "part"
        used[base] = used.get(base, 0) + 1
        label = base if used[base] == 1 else f"{base}_{used[base]}"
        lab = st.AddShape(shp, False)
        TDataStd_Name.Set_s(lab, TCollection_ExtendedString(label))
        if col is not None:
            ct.SetColor(lab, col, XCAFDoc_ColorSurf)
    w = STEPCAFControl_Writer()
    w.SetNameMode(True)
    w.SetColorMode(True)
    w.Transfer(doc, STEPControl_AsIs)
    w.Write(path)
    print(f"wrote {os.path.basename(path)}: {len(items)} parts, "
          f"{os.path.getsize(path)/1e6:.1f} MB")


def main():
    parts = read_parts(SRC)
    baked = [(n, bake(s, l), c) for n, s, l, c in parts]
    write(baked, os.path.join(OUT, "E32R40T_board_full.step"))

    # Enclosure-relevant subset: keep anything with real volume in all directions.
    simple = []
    for n, s, c in baked:
        x0, y0, z0, x1, y1, z1 = bbox(s)
        dims = sorted([x1 - x0, y1 - y0, z1 - z0])
        if dims[2] >= 2.5 and dims[0] >= 0.5:
            simple.append((n, s, c))
    write(simple, os.path.join(OUT, "E32R40T_board_simple.step"))


if __name__ == "__main__":
    main()
