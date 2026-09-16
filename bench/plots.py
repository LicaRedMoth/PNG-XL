#!/usr/bin/env python3
"""Renders the benchmark tables as charts.

Every figure this project keeps is a table, and tables hide shapes: the
still-image default sitting seventeen points away from every published number
went unnoticed for weeks because nothing ever plotted size against level.

Output is PNG rather than SVG so the files drop straight into README.md, and
every axis is labelled with its corpus and sampling -- an unlabelled chart is
worse than none, because it travels further before anyone checks it.

  bench/plots.py [outdir]        # default: docs/img
"""
import csv
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, os.pardir, "docs", "img")

INK = "#1b1b1b"
GRID = "#d8d8d8"
# Enough distinct hues that adding a corpus does not silently wrap the
# palette and give two lines the same colour, as it did at six classes.
SERIES = ["#2f6fb5", "#c4622d", "#3d8b5f", "#8a5fa8", "#b03a51",
          "#1f9099", "#8a7a20", "#5f5f5f", "#c23f8a"]

plt.rcParams.update({
    "figure.dpi": 130,
    "savefig.dpi": 130,
    "font.size": 9,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK,
    "text.color": INK,
    "xtick.color": INK,
    "ytick.color": INK,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.6,
    "figure.facecolor": "white",
    "axes.facecolor": "white",
})


def read(name):
    path = os.path.join(DATA, name)
    if not os.path.exists(path):
        return None
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh, delimiter="\t"))


def finish(fig, ax, title, subtitle, path):
    # The subtitle sits between the title and the axes, so the title needs
    # enough padding to clear it -- at equal offsets the two overprint.
    ax.set_title(title, fontsize=11, weight="bold", loc="left", pad=30)
    ax.annotate(subtitle, xy=(0, 1), xycoords="axes fraction",
                xytext=(0, 9), textcoords="offset points",
                fontsize=7.5, color="#666666", va="bottom", wrap=True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    fig.tight_layout()
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)


def load_level_files():
    """One TSV per content class, as bench/levelsweep.sh writes them.

    Keyed by the label in the filename rather than a column, so adding a corpus
    means running the sweep against it and nothing else.
    """
    out = {}
    for path in sorted(glob.glob(os.path.join(DATA, "levels_*.tsv"))):
        label = re.sub(r"^levels_|\.tsv$", "", os.path.basename(path))
        with open(path, newline="", encoding="utf-8") as fh:
            rows = [r for r in csv.DictReader(fh, delimiter="\t") if r.get("level")]
        if rows:
            out[label] = rows
    return out


def plot_levels(classes, outdir):
    """Size against encode time, one line per content class.

    Both axes matter: the level is only ever a trade of encode time for size,
    so a curve that shows size alone would argue for level 22 every time.
    """
    fig, ax = plt.subplots(figsize=(7.2, 4.3))
    for i, (name, rs) in enumerate(classes.items()):
        # levelsweep.sh reports milliseconds; the demo seed reports seconds.
        def secs(r):
            return float(r["encode_ms"]) / 1000 if "encode_ms" in r else float(r["encode_s"])
        rs.sort(key=secs)
        base_rows = [r for r in rs if r["level"] == "12"]
        if not base_rows:
            continue
        base = float(base_rows[0]["bytes"])
        xs = [secs(r) for r in rs]
        ys = [100 * float(r["bytes"]) / base for r in rs]
        c = SERIES[i % len(SERIES)]
        ax.plot(xs, ys, "-o", color=c, label=name, markersize=4.5, linewidth=1.6)
        for r, x, y in zip(rs, xs, ys):
            lv = r["level"]
            if lv in ("1", "12", "19"):
                ax.annotate("L" + lv, (x, y), textcoords="offset points",
                            xytext=(5, 5), fontsize=7, color=c)

    ax.set_xscale("log")
    ax.set_xlabel("encode time for the whole class, seconds (log scale)")
    ax.set_ylabel("size, % of level 12")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:.0f}%"))
    ax.axhline(100, color=GRID, linewidth=1, zorder=0)
    ax.legend(frameon=False, fontsize=8)
    finish(fig, ax,
           "What the zstd level buys, and what it costs",
           "L1 is what `pxltool c` produces with no arguments; each corpus "
           "normalised to its own level 12.\n"
           "Kodak rises above 100% before it falls: on BCIF-filtered "
           "photographs zstd is not monotonic in the level.",
           os.path.join(outdir, "levels.png"))


def plot_decode(rows, outdir):
    """Decode throughput against level, on synthetic content.

    The flat line was once read as a general result and is not one: measured in
    process on photographs, zstd decompression is three times slower at level 19
    than at level 1. The title says which content this is, because the earlier
    unqualified claim is exactly the mistake that needed correcting.
    """
    rows.sort(key=lambda r: int(r["level"]))
    fig, ax = plt.subplots(figsize=(6.4, 3.4))
    xs = [int(r["level"]) for r in rows]
    ys = [float(r["mb_per_s"]) for r in rows]
    ax.plot(xs, ys, "-o", color=SERIES[0], markersize=5, linewidth=1.8)
    ax.set_ylim(0, max(ys) * 1.35)
    ax.set_xticks(xs)
    ax.set_xlabel("zstd level")
    ax.set_ylabel("decode, MB/s of raw pixels")
    for x, y in zip(xs, ys):
        ax.annotate(f"{y:.0f}", (x, y), textcoords="offset points",
                    xytext=(0, 8), fontsize=7.5, ha="center", color=INK)
    finish(fig, ax,
           "On synthetic content the level does not touch decode speed",
           "The spread is noise -- but this holds for synthetic content only. "
           "On photographs\ndecompression is three times slower at L19 than at "
           "L1; see docs/BENCHMARKS.md.",
           os.path.join(outdir, "decode_levels.png"))


def plot_formats(label, rows, outdir):
    """Size against decode speed, one point per format.

    Both axes are "lower left is worse": small files to the left, fast decode
    upward, so the useful corner is top-left. Speed is logarithmic because the
    field spans three hundredfold -- on a linear axis every lossless codec but
    the fastest two collapses onto the floor.

    One chart per corpus and never a pooled one: photographs and synthetic
    stills differ by tens of points in size and by a factor of three in how
    decode responds to the level, so an average over both would describe
    neither.
    """
    fig, ax = plt.subplots(figsize=(7.0, 4.6))
    for r in rows:
        name = r["decoder"]
        x = float(r["pct_of_png"])
        y = float(r["mb_per_s"])
        is_pxl = name.startswith("PXL")
        ax.scatter(x, y, s=64 if is_pxl else 46,
                   color=SERIES[0] if is_pxl else "#7a7a7a",
                   zorder=3, edgecolor="white", linewidth=0.8)
        ax.annotate(name, (x, y), textcoords="offset points", xytext=(7, 4),
                    fontsize=8, color=INK if is_pxl else "#555555",
                    weight="bold" if is_pxl else "normal")
    ax.set_yscale("log")
    ax.set_xlabel("size, % of the source PNGs (lower is better)")
    ax.set_ylabel("decode, MB/s of RGBA output (log, higher is better)")
    ax.axvline(100, color=GRID, linewidth=1, zorder=0)
    n = rows[0].get("files", "?")
    finish(fig, ax,
           "Size against decode speed: %s" % label,
           "%s files, decoded in process to RGBA8888, single-threaded.\n"
           "libjxl and libavif are given no worker threads, which is the target's "
           "case and not a laptop's." % n,
           os.path.join(outdir, "formats_%s.png" % label))


def main():
    os.makedirs(OUT, exist_ok=True)
    made = 0
    classes = load_level_files()
    if classes:
        plot_levels(classes, OUT); made += 1
    rows = read("decode_levels.tsv")
    if rows:
        plot_decode(rows, OUT); made += 1
    for path in sorted(glob.glob(os.path.join(DATA, "formats_*.tsv"))):
        label = re.sub(r"^formats_|\.tsv$", "", os.path.basename(path))
        with open(path, newline="", encoding="utf-8") as fh:
            rows = [r for r in csv.DictReader(fh, delimiter="\t")
                    if r.get("decoder") and r["decoder"] != "decoder"]
        if rows:
            plot_formats(label, rows, OUT); made += 1
    if not made:
        print("no data files under", DATA, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
