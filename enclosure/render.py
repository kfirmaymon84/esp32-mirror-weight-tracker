"""Shaded PNG previews of the case (orthographic, painter's-algorithm render).

Run after build_case.py:  py -3 render.py
Writes out/preview_*.png.
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.collections import PolyCollection

import build_case as b
import cadquery as cq
from cadquery import exporters

OUT = b.OUT


def stl_tris(path):
    with open(path, "rb") as f:
        data = f.read()
    n = np.frombuffer(data, dtype="<u4", count=1, offset=80)[0]
    rec = np.frombuffer(data, dtype=np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")]),
                        count=n, offset=84)
    return rec["v"].astype(np.float64)


def subdivide(t, maxlen=3.0):
    """Bisect the longest edge until every triangle is small, so depth-sorting
    (painter's algorithm) stays correct for big flat faces split into slivers."""
    done = []
    for _ in range(14):
        e = np.stack([np.linalg.norm(t[:, 1] - t[:, 0], axis=1),
                      np.linalg.norm(t[:, 2] - t[:, 1], axis=1),
                      np.linalg.norm(t[:, 0] - t[:, 2], axis=1)], axis=1)
        big = e.max(axis=1) > maxlen
        done.append(t[~big])
        t = t[big]
        if not len(t):
            break
        k = e[big].argmax(axis=1)                      # longest edge index
        a = t[np.arange(len(t)), k]
        bb = t[np.arange(len(t)), (k + 1) % 3]
        c = t[np.arange(len(t)), (k + 2) % 3]
        m = (a + bb) / 2
        t = np.concatenate([np.stack([a, m, c], 1), np.stack([m, bb, c], 1)])
    done.append(t)
    return np.concatenate(done)


def mesh_of(shape, tol=0.08, name="tmp", maxlen=3.0):
    p = os.path.join(OUT, f"_{name}.stl")
    exporters.export(shape, p, tolerance=tol, angularTolerance=0.3)
    t = stl_tris(p)
    os.remove(p)
    return subdivide(t, maxlen) if maxlen else t


def rot(az, el):
    """View rotation: azimuth around Z, then elevation."""
    a, e = np.radians(az), np.radians(el)
    rz = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
    rx = np.array([[1, 0, 0], [0, np.cos(e), -np.sin(e)], [0, np.sin(e), np.cos(e)]])
    return rx @ rz


def render(items, path, az, el, title, size=(9, 6.5)):
    """items: list of (triangles Nx3x3, rgb). Camera looks down -Z' after rotation."""
    R = rot(az, el)
    light = np.array([0.35, 0.45, 0.82]); light /= np.linalg.norm(light)
    polys, cols, depth = [], [], []
    for tris, rgb in items:
        v = tris @ R.T
        nrm = np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
        ln = np.linalg.norm(nrm, axis=1); ok = ln > 1e-9
        v, nrm, ln = v[ok], nrm[ok], ln[ok]
        nrm = nrm / ln[:, None]
        front = nrm[:, 2] > 0                      # back-face culling
        v, nrm = v[front], nrm[front]
        shade = 0.30 + 0.70 * np.clip(nrm @ light, 0, 1)
        c = np.clip(np.outer(shade, rgb), 0, 1)
        polys.append(v[:, :, :2]); cols.append(c); depth.append(v[:, :, 2].mean(axis=1))
    P = np.concatenate(polys); C = np.concatenate(cols); D = np.concatenate(depth)
    order = np.argsort(D)
    fig, ax = plt.subplots(figsize=size, dpi=130)
    ax.add_collection(PolyCollection(P[order], facecolors=C[order], edgecolors=C[order], linewidths=0.15))
    allp = P.reshape(-1, 2)
    pad = 6
    ax.set_xlim(allp[:, 0].min() - pad, allp[:, 0].max() + pad)
    ax.set_ylim(allp[:, 1].min() - pad, allp[:, 1].max() + pad)
    ax.set_aspect("equal"); ax.axis("off")
    ax.set_title(title, fontsize=12)
    fig.tight_layout(); fig.savefig(path, facecolor="white"); plt.close(fig)
    print("wrote", os.path.basename(path))


def main():
    shell = b.front_shell().val()
    cover = b.back_cover().val()
    stand = b.stand().val()
    ph = b.placeholders()
    board = cq.Compound.makeCompound(b.board_solids())

    T_shell, T_cover, T_stand = mesh_of(shell, name="s"), mesh_of(cover, name="c"), mesh_of(stand, name="t")
    T_board = mesh_of(board, tol=0.2, name="b", maxlen=6.0)
    T_ph = {k: mesh_of(v.val(), name=k) for k, v in ph.items()}

    DARK, MID, GREEN, ORANGE = (0.22, 0.24, 0.27), (0.42, 0.44, 0.48), (0.15, 0.55, 0.30), (0.95, 0.62, 0.15)

    # 1. Front of the closed case.
    render([(T_shell, DARK)], os.path.join(OUT, "preview_front.png"), 0, 25,
           "Front - display window, ToF window (top), speaker grille (bottom)")

    # 2. Inside of the front shell with the board removed.
    render([(T_shell, MID)], os.path.join(OUT, "preview_shell_inside.png"), 180, -60,
           "Front shell, inside - PCB posts, ToF pocket, speaker pocket, corner pillars")

    # 3. Exploded assembly.
    def off(t, dz):
        t = t.copy(); t[:, :, 2] += dz; return t
    items = [(off(T_shell, 0), DARK), (off(T_board, -30), GREEN),
             (off(T_ph["tof"], 0), ORANGE), (off(T_ph["speaker"], 0), ORANGE),
             (off(T_ph["battery"], -55), ORANGE), (off(T_cover, -75), MID)]
    render(items, os.path.join(OUT, "preview_exploded.png"), 25, -62,
           "Exploded - front shell / board / battery / back cover", size=(9, 9))

    # 4. Back cover, outside.
    render([(T_cover, MID)], os.path.join(OUT, "preview_back.png"), 180, 180 - 25,
           "Back - keyholes and BOOT/RESET pin holes (snap-in, no screws)")

    # 5. Device sitting on the stand (side-ish view).
    import math
    th = math.radians(15.0)
    asm = cq.Workplane().add(shell).union(cq.Workplane().add(cover)).val()
    # put the device's back face on the stand plate: bottom-back edge at (y=0, h=3)
    dev = asm.translate(cq.Vector(-b.CX, b.OUT_Y, -b.Z_BO))          # back face z=0, bottom y=0
    dev = dev.rotate(cq.Vector(0, 0, 0), cq.Vector(1, 0, 0), 90)       # upright: screen-up -> +Z, front -> -Y
    dev = dev.rotate(cq.Vector(0, 0, 0), cq.Vector(1, 0, 0), -15)      # lean back 15 deg
    dev = dev.translate(cq.Vector(0, 0, 3.0))
    T_dev = mesh_of(dev, name="d")
    # el=-90 looks horizontally at a Z-up scene; az turns it to a 3/4 view.
    render([(T_stand, (0.55, 0.35, 0.20)), (T_dev, DARK)], os.path.join(OUT, "preview_on_stand.png"),
           -35, -75, "On the desk stand (pegs locked in the keyholes)")
    render([(T_stand, (0.55, 0.35, 0.20)), (T_dev, DARK)], os.path.join(OUT, "preview_on_stand_side.png"),
           -90, -90, "Side view - 15 deg lean, device seated on the lip")


if __name__ == "__main__":
    main()
