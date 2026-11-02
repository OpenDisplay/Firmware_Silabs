#!/usr/bin/env python3
"""Measure two 4x4 palette grids from one hand-held photo and chart them.

Built for a side-by-side shot of two panels showing the same 4x4 grid: the left
running the optimised palette, the right the stock one. It samples every cell,
white-balances each panel against its OWN white entry, converts to CIELAB, and
emits a comparison chart plus the numbers.

WHY PER-PANEL WHITE BALANCE. A hand-held photo under room light has no colour
reference, so absolute colorimetry is not available. Both palettes contain the
same white entry, though, so normalising each panel against its own white makes
the two directly comparable to each other and to the rig measurements, which
normalise the same way. Differences between the panels are then real; absolute
L*a*b* values are not claimed to be.

The sampling inset is generous because these are photographed cells with soft
edges and visible glare: a cell's centre is trustworthy, its border is not.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

GRID = 4

# Palette encodings, for labelling the output. Entry 15 is the only difference.
PALETTE_OPTIMISED = [(2, 0, 0), (1, 1, 1), (2, 1, 1), (3, 1, 1),
                     (0, 1, 1), (1, 0, 3), (1, 2, 1), (2, 0, 2),
                     (2, 1, 3), (2, 3, 1), (3, 1, 2), (0, 2, 1),
                     (0, 3, 1), (3, 2, 1), (0, 1, 0), (2, 1, 2)]
PALETTE_STOCK = PALETTE_OPTIMISED[:15] + [(3, 0, 1)]
WHITE_ENTRY = 1          # (1,1,1) in both palettes


def srgb_to_lab(rgb):
    import numpy as np
    c = np.asarray(rgb, dtype=np.float64) / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883])
    d = 6.0 / 29.0
    f = np.where(xyz > d ** 3, np.cbrt(xyz), xyz / (3 * d * d) + 4.0 / 29.0)
    return np.stack([116 * f[..., 1] - 16,
                     500 * (f[..., 0] - f[..., 1]),
                     200 * (f[..., 1] - f[..., 2])], axis=-1)


def sample_grid(img, rect, inset=0.3):
    """rect = (x0, y0, x1, y1) of the ACTIVE AREA -> 16 mean sRGB triples."""
    import numpy as np
    x0, y0, x1, y1 = rect
    cw, ch = (x1 - x0) / GRID, (y1 - y0) / GRID
    ix, iy = cw * inset, ch * inset
    out = []
    for i in range(GRID * GRID):
        r, c = divmod(i, GRID)
        sx0, sx1 = int(x0 + c * cw + ix), int(x0 + (c + 1) * cw - ix)
        sy0, sy1 = int(y0 + r * ch + iy), int(y0 + (r + 1) * ch - iy)
        patch = img[sy0:sy1, sx0:sx1].reshape(-1, 3).astype(np.float64)
        out.append(patch.mean(axis=0))
    return np.array(out)


def analyse(rgb_means, label):
    import numpy as np
    white = rgb_means[WHITE_ENTRY]
    if white.max() <= 0:
        raise SystemExit(f"{label}: white entry sampled as black; the rect is wrong")
    balanced = np.clip(rgb_means / white * 255.0, 0, 255)
    lab = srgb_to_lab(balanced)
    C = np.hypot(lab[:, 1], lab[:, 2])
    H = (np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) + 360) % 360
    try:
        from scipy.spatial import ConvexHull
        hull = ConvexHull(lab).volume
    except Exception:
        hull = None
    return {"label": label, "rgb": balanced, "lab": lab, "C": C, "H": H,
            "hull": hull}


def chart(results, palettes, out_path):
    """Side-by-side swatch chart with the numbers under each cell."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    fig, axes = plt.subplots(1, len(results), figsize=(7.2 * len(results), 8.6))
    if len(results) == 1:
        axes = [axes]
    for ax, res, pal in zip(axes, results, palettes):
        ax.set_xlim(0, GRID); ax.set_ylim(GRID, 0); ax.set_aspect("equal")
        ax.set_xticks([]); ax.set_yticks([])
        for i in range(GRID * GRID):
            r, c = divmod(i, GRID)
            col = np.clip(res["rgb"][i] / 255.0, 0, 1)
            ax.add_patch(plt.Rectangle((c, r), 1, 1, facecolor=col, edgecolor="white", lw=1.5))
            L, a, b = res["lab"][i]
            # Label in whichever of black/white reads against the swatch.
            txt = "white" if L < 55 else "black"
            ax.text(c + 0.5, r + 0.30, f"{i}  {tuple(pal[i])}", ha="center",
                    va="center", color=txt, fontsize=8, weight="bold")
            ax.text(c + 0.5, r + 0.55, f"L*{L:.0f}", ha="center", va="center",
                    color=txt, fontsize=8)
            ax.text(c + 0.5, r + 0.76, f"C*{res['C'][i]:.0f}  h{res['H'][i]:.0f}°",
                    ha="center", va="center", color=txt, fontsize=7)
        hull = f"  hull {res['hull']:.0f}" if res["hull"] else ""
        ax.set_title(f"{res['label']}\nmean C* {res['C'].mean():.1f}   "
                     f"C*>=10: {(res['C'] >= 10).sum()}/16{hull}", fontsize=11)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    print(f"wrote {out_path}")


def main() -> int:
    import numpy as np
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("--left", required=True,
                    help="x0,y0,x1,y1 of the LEFT panel's active area")
    ap.add_argument("--right", help="x0,y0,x1,y1 of the RIGHT panel's active area")
    ap.add_argument("--inset", type=float, default=0.3)
    ap.add_argument("--out", default="palette_comparison.png")
    ap.add_argument("--json", default="palette_photo_measured.json")
    a = ap.parse_args()

    from PIL import Image
    img = np.array(Image.open(a.photo).convert("RGB"))
    print(f"photo {img.shape[1]}x{img.shape[0]}")

    rects = [("optimised (swap15)", tuple(int(v) for v in a.left.split(",")),
              PALETTE_OPTIMISED)]
    if a.right:
        rects.append(("stock fd", tuple(int(v) for v in a.right.split(",")),
                      PALETTE_STOCK))

    results, palettes = [], []
    for label, rect, pal in rects:
        means = sample_grid(img, rect, a.inset)
        res = analyse(means, label)
        results.append(res); palettes.append(pal)
        print(f"\n{label}  rect={rect}")
        print("  #  codes       L*     a*     b*    C*    h")
        for i in range(16):
            L, A, B = res["lab"][i]
            print(f" {i:2d}  {str(tuple(pal[i])):10s} {L:6.1f} {A:6.1f} {B:6.1f} "
                  f"{res['C'][i]:5.1f} {res['H'][i]:5.0f}")
        print(f"  mean C* {res['C'].mean():.2f}   C*>=10 {(res['C']>=10).sum()}/16"
              + (f"   hull {res['hull']:.0f}" if res["hull"] else ""))

    if len(results) == 2:
        import numpy as _np
        d = _np.linalg.norm(results[0]["lab"] - results[1]["lab"], axis=1)
        print(f"\nper-entry dE76 between the two panels: "
              f"min {d.min():.1f} mean {d.mean():.1f} max {d.max():.1f}")
        print(f"  entry 15 (the swap): dE76 {d[15]:.1f}")
        if results[0]["hull"] and results[1]["hull"]:
            g = 100 * (results[0]["hull"] / results[1]["hull"] - 1)
            print(f"  gamut hull optimised vs stock: {g:+.1f}%")

    chart(results, palettes, a.out)
    Path(a.json).write_text(json.dumps(
        {r["label"]: {"lab": r["lab"].tolist(), "C": r["C"].tolist(),
                      "hull": r["hull"]} for r in results}, indent=1))
    print(f"wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
