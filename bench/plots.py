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
SERIES = ["#2f6fb5", "#c4622d", "#3d8b5f", "#8a5fa8", "#b03a51"]

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
           "L1 is what `pxltool c` produces with no arguments; "
           "one line per corpus, each normalised to its own level 12.",
           os.path.join(outdir, "levels.png"))


def plot_decode(rows, outdir):
    """Decode throughput against level -- the flat line is the whole point."""
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
           "Compression level does not touch decode speed",
           "The spread is noise. 92.8 MB of stream, 3 passes.",
           os.path.join(outdir, "decode_levels.png"))


def main():
    os.makedirs(OUT, exist_ok=True)
    made = 0
    classes = load_level_files()
    if classes:
        plot_levels(classes, OUT); made += 1
    rows = read("decode_levels.tsv")
    if rows:
        plot_decode(rows, OUT); made += 1
    if not made:
        print("no data files under", DATA, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
