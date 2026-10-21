# Board STEP inspector: mounting holes (PCB cylinders) + every part > 2 mm with its bbox.
import sys, math
from OCP.STEPCAFControl import STEPCAFControl_Reader
from OCP.TDocStd import TDocStd_Document
from OCP.TCollection import TCollection_ExtendedString
from OCP.XCAFDoc import XCAFDoc_DocumentTool
from OCP.TDF import TDF_LabelSequence, TDF_Label
from OCP.TDataStd import TDataStd_Name
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib
from OCP.TopLoc import TopLoc_Location
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE
from OCP.TopoDS import TopoDS
from OCP.BRepAdaptor import BRepAdaptor_Surface
from OCP.GeomAbs import GeomAbs_Cylinder

doc = TDocStd_Document(TCollection_ExtendedString("doc"))
r = STEPCAFControl_Reader(); r.SetNameMode(True)
r.ReadFile(sys.argv[1]); r.Transfer(doc)
st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
def name(lab):
    n = TDataStd_Name()
    return n.Get().ToExtString() if lab.FindAttribute(TDataStd_Name.GetID_s(), n) else "?"
rows = []
def walk(lab, loc):
    if st.IsReference_s(lab):
        ref = TDF_Label(); st.GetReferredShape_s(lab, ref)
        walk(ref, loc.Multiplied(st.GetLocation_s(lab))); return
    if st.IsAssembly_s(lab):
        k = TDF_LabelSequence(); st.GetComponents_s(lab, k)
        for i in range(1, k.Length() + 1): walk(k.Value(i), loc)
    else:
        s = st.GetShape_s(lab).Moved(loc)
        b = Bnd_Box(); BRepBndLib.Add_s(s, b)
        if not b.IsVoid(): rows.append((name(lab), s, b.Get()))
roots = TDF_LabelSequence(); st.GetFreeShapes(roots)
for i in range(1, roots.Length() + 1): walk(roots.Value(i), TopLoc_Location())

pcb = max(rows, key=lambda r: (r[2][3]-r[2][0])*(r[2][4]-r[2][1]) if abs(r[2][2]+1.6) < 0.01 else 0)
print("PCB bbox", [round(v, 2) for v in pcb[2]])
holes = {}
ex = TopExp_Explorer(pcb[1], TopAbs_FACE)
while ex.More():
    f = TopoDS.Face_s(ex.Current()); a = BRepAdaptor_Surface(f)
    if a.GetType() == GeomAbs_Cylinder:
        c = a.Cylinder(); rr = c.Radius(); p = c.Location()
        if 0.8 < rr < 3.0:
            key = (round(p.X(), 1), round(p.Y(), 1), round(rr, 2))
            holes[key] = 1
    ex.Next()
print("PCB cylindrical holes (x, y, radius):")
for k in sorted(holes): print("  ", k)

print("\nParts > 2 mm:")
for n, s, (x0, y0, z0, x1, y1, z1) in sorted(rows, key=lambda r: r[2][0]):
    if max(x1-x0, y1-y0) < 2.0: continue
    nm = n.encode("ascii", "replace").decode()[:34]
    print(f"{nm:34s} x[{x0:7.2f},{x1:7.2f}] y[{y0:6.2f},{y1:6.2f}] z[{z0:6.2f},{z1:6.2f}]")
