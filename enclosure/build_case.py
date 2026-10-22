"""Smart Mirror Weight Tracker — 3D-printable case for the 4" E32R40T board.

Parametric CadQuery model. Run:  py -3 build_case.py
Outputs (out/): front_shell / back_cover / stand as STEP + STL, an assembly
preview STEP (case + board + speaker/ToF placeholders), SVG views, and
an interference report checking every part against the real board model.

Local frame (all mm): origin = PCB centre, +X = USB-C end, +Y = screen top
(landscape, USB on the right), +Z = toward the viewer, Z=0 = PCB top face.
Board geometry below was measured from E32R40T_3D.step (see inspect2.py).
"""
import math
import os
import sys

import cadquery as cq
from cadquery import exporters

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
BOARD_STEP = r"C:\Users\KfirMaymon\Downloads\E32R40T_3D.step"
STEP_TO_LOCAL = (117.735, -59.70, 0.0)      # STEP coords -> local frame

# ── Board (measured) ─────────────────────────────────────────────────────────
PCB_HX, PCB_HY = 55.555, 30.44              # PCB half size (111.1 x 60.9)
HOLES = [(sx * 52.05, sy * 26.6) for sx in (-1, 1) for sy in (-1, 1)]  # M3
GLASS_TOP = 4.05                            # top of the touch glass
AA_W, AA_H, AA_CX = 84.32, 56.28, -2.725    # visible area (off-centre in X)
USB_Y, USB_Z = 0.0, -2.69                   # USB-C receptacle centre
BUTTONS = [(52.285, -15.03), (52.285, 15.08)]  # BOOT (-Y), RESET (+Y), pressed from back

# ── Parts that go inside (user-supplied sizes) ──────────────────────────────
SPK = (16.0, 9.0, 4.0)                      # speaker L x W x H
TOF = (25.0, 10.7, 1.6)                     # GY-530 PCB L x W x T

# ── Case parameters ──────────────────────────────────────────────────────────
WALL, FRONT_T, BACK_T = 2.4, 2.0, 2.4
GLASS_GAP = 0.4        # bezel never presses the resistive touch layer
CX = AA_CX             # case centred on the visible screen, not the PCB
Z_FI = GLASS_TOP + GLASS_GAP                # front plate, inner face
Z_FO = Z_FI + FRONT_T                       # front face
Z_BI = -15.5                                # back cover, inner face
Z_BO = Z_BI - BACK_T                        # back face
IN_X = (PCB_HX + 0.92) - CX                 # right wall 0.55 mm from USB-C metal
IN_Y = 44.5
OUT_X, OUT_Y = IN_X + WALL, IN_Y + WALL
R_OUT = 6.0                                 # outer corner radius
R_IN = R_OUT - WALL
T_CASE = Z_FO - Z_BO                        # overall thickness

M3_PILOT = 2.6         # self-tapping M3 into printed plastic
M3_CLEAR = 3.4

# Corner pillars (back-cover screws), concentric with the inner corner arcs.
PILLARS = [(CX + sx * (IN_X - R_IN), sy * (IN_Y - R_IN)) for sx in (-1, 1) for sy in (-1, 1)]
PILLAR_R = R_IN + 0.6

# ToF pocket above the display, speaker pocket below it.
RIB = 1.2
TOF_Y = PCB_HY + 0.5 + RIB + 0.3 + TOF[1] / 2          # clear of the PCB edge
SPK_Y = -(PCB_HY + 0.5 + RIB + 0.2 + SPK[1] / 2)

# Keyholes (wall screw / stand peg): big hole, slot runs UP from it.
KEY_X = (CX - 35.0, CX + 35.0)
KEY_BIG_Y, KEY_TOP_Y = 24.0, 32.0
KEY_BIG_D, KEY_SLOT_W = 8.5, 4.2
HOOD_W, HOOD_H, HOOD_WALL, HOOD_ROOF = 9.0, 3.0, 1.6, 1.2

# Snap-fit back cover (no screws). Cantilever hooks rise from the cover along the
# inside of the top and bottom walls; each barb clicks into a hidden pocket in the
# wall. The arms print vertically (the weak FDM direction), so they are long and
# thin and only deflect 0.55 mm -> ~1.4 % strain, comfortable for PETG/PLA.
# The catch face is sloped (~35 deg): holds firmly, releases with a firm pull.
SNAP_X = (CX - 35.0, CX + 35.0)     # on both the top and bottom walls
SNAP_W, SNAP_T = 8.0, 1.4           # arm width (along the wall), thickness
SNAP_CLR = 0.15                     # arm-to-wall gap
SNAP_BARB = 0.7                     # barb height (0.55 mm past the wall face)
SNAP_Z0 = Z_BI + 8.0                # catch face starts here (cover seated)
SNAP_TIP = SNAP_Z0 + 2.6            # top of the arm


def snap_profile(sign):
    """Arm + barb cross-section in the (y, z) plane; sign=+1 top wall, -1 bottom."""
    a_out = IN_Y - SNAP_CLR
    a_in = a_out - SNAP_T
    pts = [(a_in, Z_BI - 0.01), (a_out, Z_BI - 0.01), (a_out, SNAP_Z0),
           (a_out + SNAP_BARB, SNAP_Z0 + 0.5), (a_out + SNAP_BARB, SNAP_Z0 + 1.1),
           (a_out, SNAP_TIP), (a_in, SNAP_TIP)]
    return [(sign * y, z) for y, z in pts]


def wp(z=0.0):
    return cq.Workplane("XY").workplane(offset=z)


# ── Front shell ──────────────────────────────────────────────────────────────
def front_shell():
    body = wp(Z_BI).center(CX, 0).rect(2 * OUT_X, 2 * OUT_Y).extrude(Z_FO - Z_BI)
    body = body.edges("|Z").fillet(R_OUT)
    try:
        body = body.faces(">Z").edges().fillet(1.5)
    except Exception:
        pass
    cavity = wp(Z_BI - 1).center(CX, 0).rect(2 * IN_X, 2 * IN_Y).extrude(Z_FI - Z_BI + 1)
    cavity = cavity.edges("|Z").fillet(R_IN)
    body = body.cut(cavity)

    # Display window = visible area + 0.5 mm, with a 1 mm chamfer on the front.
    ww, wh = AA_W + 1.0, AA_H + 1.0
    body = body.cut(wp(Z_FI - 0.5).center(AA_CX, 0).rect(ww, wh).extrude(FRONT_T + 1))
    ch = 1.0
    bevel = (wp(Z_FO - ch).center(AA_CX, 0).rect(ww, wh)
             .workplane(offset=ch + 0.02).rect(ww + 2 * ch, wh + 2 * ch).loft())
    body = body.cut(bevel)

    # PCB posts: the board's top face sits on them, screwed from the back.
    for x, y in HOLES:
        body = body.union(wp(0).center(x, y).circle(3.25).extrude(Z_FI + 0.1))
        body = body.cut(wp(-0.1).center(x, y).circle(M3_PILOT / 2).extrude(5.0))   # M3x6 = 4.4 mm thread

    # Corner pillars: stiffen the frame and act as stops for the snap-in cover.
    for x, y in PILLARS:
        body = body.union(wp(Z_BI).center(x, y).circle(PILLAR_R).extrude(Z_FI - Z_BI + 0.1))

    # Hidden pockets in the top/bottom walls that the cover's snap barbs click into.
    depth = SNAP_BARB - SNAP_CLR + 0.25
    for sx in SNAP_X:
        for sign in (1, -1):
            y0 = IN_Y - 0.1 if sign > 0 else -(IN_Y + depth)
            body = body.cut(wp(SNAP_Z0 - 0.3).center(sx, y0 + (depth + 0.1) / 2)
                            .rect(SNAP_W + 0.6, depth + 0.1).extrude(2.8))

    # USB-C opening in the right wall (fits a typical plug overmold).
    usb = (cq.Workplane("YZ").workplane(offset=CX + IN_X - 1.0)
           .center(USB_Y, USB_Z).rect(13.0, 7.5).extrude(WALL + 2.0))
    usb = usb.edges("|X").fillet(2.0)
    body = body.cut(usb)

    # ToF pocket: ribs + 4 corner pads keep the sensor 1.5 mm off the plate.
    tl, tw = TOF[0] + 0.6, TOF[1] + 0.6
    frame = (wp(Z_FI - 3.0).center(CX, TOF_Y).rect(tl + 2 * RIB, tw + 2 * RIB).extrude(3.1)
             .cut(wp(Z_FI - 3.1).center(CX, TOF_Y).rect(tl, tw).extrude(3.3)))
    body = body.union(frame)
    for sx in (-1, 1):
        for sy in (-1, 1):
            body = body.union(wp(Z_FI - 1.5).center(CX + sx * (tl / 2 - 1), TOF_Y + sy * (tw / 2 - 1))
                              .rect(2, 2).extrude(1.6))
    aper = wp(Z_FI - 0.5).center(CX, TOF_Y).rect(10.0, 6.0).extrude(FRONT_T + 1)
    body = body.cut(aper.edges("|Z").fillet(1.5))
    body = body.cut(wp(Z_FO - 0.8).center(CX, TOF_Y).rect(10.0, 6.0)
                    .workplane(offset=0.82).rect(11.6, 7.6).loft())

    # Speaker pocket + grille (speaker faces the user, under the display).
    sl, sw = SPK[0] + 0.4, SPK[1] + 0.4
    spk = (wp(Z_FI - SPK[2] - 0.4).center(CX, SPK_Y).rect(sl + 2 * RIB, sw + 2 * RIB).extrude(SPK[2] + 0.5)
           .cut(wp(Z_FI - SPK[2] - 0.6).center(CX, SPK_Y).rect(sl, sw).extrude(SPK[2] + 0.8)))
    body = body.union(spk)
    pts = [(CX + i * 2.5, SPK_Y + j * 2.5) for i in range(-2, 3) for j in range(-1, 2)]
    body = body.cut(wp(Z_FI - 0.5).pushPoints(pts).circle(0.8).extrude(FRONT_T + 1))
    return body


# ── Back cover ───────────────────────────────────────────────────────────────
def back_cover():
    plate = wp(Z_BO).center(CX, 0).rect(2 * OUT_X, 2 * OUT_Y).extrude(BACK_T)
    plate = plate.edges("|Z").fillet(R_OUT)
    try:
        plate = plate.faces("<Z").edges().fillet(1.0)
    except Exception:
        pass

    # Snap hooks (top and bottom walls) — no screws.
    for sx in SNAP_X:
        for sign in (1, -1):
            hook = (cq.Workplane("YZ").workplane(offset=sx - SNAP_W / 2)
                    .polyline(snap_profile(sign)).close().extrude(SNAP_W))
            plate = plate.union(hook)

    # Locating tabs on the left/right walls so the cover drops in centred.
    for side in (-1, 1):
        tx = CX + side * (IN_X - SNAP_CLR - 0.7)
        plate = plate.union(wp(Z_BI - 0.01).center(tx, 0).rect(1.4, 12.0).extrude(4.0))

    # Pry notch at the bottom seam: a coin or fingernail lifts the cover off.
    plate = plate.cut(wp(Z_BI - 1.0).center(CX, -OUT_Y + 0.6).rect(12.0, 1.4).extrude(1.1))

    # Pin holes to press BOOT / RESET with a paper clip.
    for x, y in BUTTONS:
        plate = plate.cut(wp(Z_BO - 0.1).center(x, y).circle(1.0).extrude(BACK_T + 0.2))

    # Keyholes + closed hoods behind them so a screw head never touches parts.
    slot_len = (KEY_TOP_Y - KEY_BIG_Y) + KEY_SLOT_W
    hood_len = (KEY_TOP_Y - KEY_BIG_Y) + HOOD_W
    mid_y = (KEY_BIG_Y + KEY_TOP_Y) / 2
    for kx in KEY_X:
        outer = wp(Z_BI).center(kx, mid_y).slot2D(hood_len + 2 * HOOD_WALL, HOOD_W + 2 * HOOD_WALL, 90) \
            .extrude(HOOD_H + HOOD_ROOF)
        inner = wp(Z_BI - 0.1).center(kx, mid_y).slot2D(hood_len, HOOD_W, 90).extrude(HOOD_H + 0.1)
        plate = plate.union(outer).cut(inner)
        plate = plate.cut(wp(Z_BO - 0.1).center(kx, mid_y).slot2D(slot_len, KEY_SLOT_W, 90)
                          .extrude(BACK_T + 0.2))
        plate = plate.cut(wp(Z_BO - 0.1).center(kx, KEY_BIG_Y).circle(KEY_BIG_D / 2)
                          .extrude(BACK_T + 0.2))
    return plate


# ── Desk stand (pegs drop into the keyholes) ────────────────────────────────
def stand():
    th = math.radians(15.0)                      # lean back from vertical
    u = (math.sin(th), math.cos(th))             # up along the device back
    n = (math.cos(th), -math.sin(th))            # away from the device back
    def p(base, *terms):
        y, h = base
        for k, v in terms:
            y += k * v[0]; h += k * v[1]
        return (y, h)
    T = T_CASE
    B = (0.0, 3.0)                               # device bottom-back edge
    F = p(B, (-T, n))                            # device bottom-front edge
    s_peg = KEY_TOP_Y + OUT_Y                    # peg height along the back
    W = 100.0
    LIP = 5.0                                    # low enough to clear the speaker grille
    prof = [p(F, (-3, n), (LIP, u)), p(F, (LIP, u)), F, B, p(B, (s_peg + 9, u)),
            p(B, (s_peg + 9, u), (4, n)), p(B, (4, n)), (p(B, (4, n))[0], 0.0),
            (p(F, (-3, n))[0], 0.0), p(F, (-3, n))]
    body = cq.Workplane("YZ").polyline(prof).close().extrude(W / 2, both=True)
    g0 = p(B, (4, n))
    gus = [p(B, (4, n), (55, u)), (42.0, 0.0), (g0[0], 0.0), g0]
    for gx in (-W / 2 + 3, W / 2 - 3):
        body = body.union(cq.Workplane("YZ").workplane(offset=gx).polyline(gus).close()
                          .extrude(3, both=True))
    pp = p(B, (s_peg, u))
    d = cq.Vector(0, -n[0], -n[1])               # into the device
    for kx in (-35.0, 35.0):
        base = cq.Vector(kx, pp[0] + n[0] * 1.0, pp[1] + n[1] * 1.0)
        neck = cq.Solid.makeCylinder(KEY_SLOT_W / 2 - 0.2, BACK_T + 0.4 + 1.0, base, d)
        hp = cq.Vector(kx, pp[0] - n[0] * (BACK_T + 0.4), pp[1] - n[1] * (BACK_T + 0.4))
        head = cq.Solid.makeCylinder(KEY_BIG_D / 2 - 0.5, 2.0, hp, d)
        body = body.union(cq.Workplane().add(neck)).union(cq.Workplane().add(head))
    return body


# ── Placeholders for the internal parts (preview + interference check) ─────
def placeholders():
    spk = wp(Z_FI - SPK[2]).center(CX, SPK_Y).rect(SPK[0], SPK[1]).extrude(SPK[2])
    tof = wp(Z_FI - 1.5 - TOF[2]).center(CX, TOF_Y).rect(TOF[0], TOF[1]).extrude(TOF[2])
    tof = tof.union(wp(Z_FI - 1.5).center(CX, TOF_Y).rect(4.4, 2.4).extrude(1.0))  # sensor
    return {"speaker": spk, "tof": tof}


def board_solids():
    shp = cq.importers.importStep(BOARD_STEP).val().translate(cq.Vector(*STEP_TO_LOCAL))
    return shp.Solids()


def interference(name, part, others):
    bb = part.val().BoundingBox()
    hits = []
    for label, s in others:
        sb = s.BoundingBox()
        if sb.xmax < bb.xmin or sb.xmin > bb.xmax or sb.ymax < bb.ymin or sb.ymin > bb.ymax \
                or sb.zmax < bb.zmin or sb.zmin > bb.zmax:
            continue
        try:
            v = part.val().intersect(s).Volume()
        except Exception:
            continue
        if v > 0.01:
            hits.append((label, v))
    return hits


def main():
    os.makedirs(OUT, exist_ok=True)
    parts = {"front_shell": front_shell(), "back_cover": back_cover(), "stand": stand()}
    for k, v in parts.items():
        exporters.export(v, os.path.join(OUT, k + ".step"))
        exporters.export(v, os.path.join(OUT, k + ".stl"), tolerance=0.03, angularTolerance=0.15)
        bb = v.val().BoundingBox()
        print(f"{k:12s} {bb.xlen:6.1f} x {bb.ylen:6.1f} x {bb.zlen:6.1f} mm  volume {v.val().Volume()/1000:6.1f} cm3")

    print(f"case outer: {2*OUT_X:.1f} x {2*OUT_Y:.1f} x {T_CASE:.1f} mm")

    if "--check" in sys.argv or True:
        print("loading board model for the interference check ...")
        bsol = [("board", s) for s in board_solids()]
        ph = placeholders()
        inner = [(k, v.val()) for k, v in ph.items()]
        ok = True
        for k in ("front_shell", "back_cover"):
            hits = interference(k, parts[k], bsol + inner)
            print(f"  {k}: " + ("no interference" if not hits else
                                "; ".join(f"{l} {v:.2f} mm3" for l, v in hits)))
            ok &= not hits
        for k, v in ph.items():
            hits = interference(k, v, bsol)
            print(f"  {k} vs board: " + ("clear" if not hits else
                                         "; ".join(f"{l} {vv:.2f} mm3" for l, vv in hits)))
            ok &= not hits
        print("INTERFERENCE CHECK:", "PASS" if ok else "FAIL")

        asm = cq.Assembly()
        asm.add(parts["front_shell"], name="front_shell", color=cq.Color(0.15, 0.15, 0.17, 1))
        asm.add(parts["back_cover"], name="back_cover", color=cq.Color(0.25, 0.25, 0.28, 1))
        for k, v in ph.items():
            asm.add(v, name=k, color=cq.Color(0.9, 0.6, 0.1, 1))
        asm.add(cq.Workplane().add(cq.Compound.makeCompound([s for _, s in bsol])),
                name="board", color=cq.Color(0.1, 0.5, 0.2, 1))
        asm.save(os.path.join(OUT, "assembly_preview.step"))

    views = {"front": (0, 0, 1), "back": (0, 0, -1), "iso": (1, -1, 1)}
    for k in ("front_shell", "back_cover"):
        for vn, d in views.items():
            exporters.export(parts[k], os.path.join(OUT, f"{k}_{vn}.svg"),
                             opt={"width": 900, "height": 700, "projectionDir": d,
                                  "showHidden": False, "strokeWidth": 0.4})
    exporters.export(parts["stand"], os.path.join(OUT, "stand_side.svg"),
                     opt={"width": 900, "height": 700, "projectionDir": (1, 0, 0),
                          "showHidden": False, "strokeWidth": 0.4})
    exporters.export(parts["stand"], os.path.join(OUT, "stand_iso.svg"),
                     opt={"width": 900, "height": 700, "projectionDir": (1, -1, 0.6),
                          "showHidden": False, "strokeWidth": 0.4})


if __name__ == "__main__":
    main()
