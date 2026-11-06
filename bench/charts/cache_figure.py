#!/usr/bin/env python3
"""The fast-postgres vs PgCache figure, drawn through a chosen style.

`cache.py` and `style_lab.py` both read the same log; this module keeps the one
figure they draw from it. The caller supplies the largest-size entry from
`cache.parse` (the `large` and `small` workloads) and a style name from
`styles.STYLES`.

    from cache_figure import render
    render("brutal", data[max(data)], env, out_dir, "pgcache-read.png")
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, NullFormatter  # noqa: E402

import styles  # noqa: E402

SERIES = ("fast-postgres", "PgCache (warm)")
TITLE = "fast-postgres against a result cache"
SUBTITLE = "1M rows · the same output, priced two ways"
READ = "the winner depends on whether the query ships rows or skips work."


def fmt_rows(v, _pos=None):
    return f"{v / 1e6:.2f}M"


def fmt_rows_axis(v, _pos=None):
    return f"{v / 1e6:g}M"


def fmt_ms(v, _pos=None):
    return f"{v:.1f}" if v >= 10 else f"{v:.2f}"


def fmt_ms_axis(v, _pos=None):
    return f"{v:g}"


def draw_panel(ax, style, values, labels, ylabel, log_scale, axis_formatter):
    width = style.bar_width
    step = width * style.bar_gap
    positions = [i * step for i in range(len(values))]
    if log_scale:
        ax.set_yscale("log")
        ax.set_ylim(0.7, 130)
        ax.yaxis.set_minor_formatter(NullFormatter())
    bars = []
    for i, (value, label) in enumerate(zip(values, labels)):
        drawn = style.bar(ax, positions[i], value, width, i)
        style.bar_label(ax, drawn, label, i)
        bars.append(drawn[0])

    ax.set_xlim(-step * 0.9, positions[-1] + step * 0.9)
    ax.set_xticks(positions)
    ax.set_xticklabels(SERIES)
    style.x_labels(ax, SERIES)
    ax.set_ylabel(ylabel)
    ax.yaxis.set_major_formatter(FuncFormatter(axis_formatter))
    ax.margins(y=style.y_margin)
    style.frame(ax)
    return bars


def render(style_name, data, env, out_dir, filename):
    """Draw the comparison; `data` is one size from `cache.parse`."""
    style = styles.STYLES[style_name]

    fig = plt.figure(figsize=(10.4, 5.8))
    style.setup(fig)
    ax_scan = fig.add_axes((0.085, 0.34, 0.36, 0.38))
    ax_agg = fig.add_axes((0.575, 0.34, 0.36, 0.38))
    note_ax = fig.add_axes((0.06, 0.085, 0.88, 0.14))

    scan = data["large"]
    agg = data["small"]
    scan_values = (scan["fastpostgres"], scan["pgcache"])
    agg_values = (agg["fastpostgres"], agg["pgcache"])
    scan_pct = scan["pgcache"] / scan["fastpostgres"] * 100
    agg_pct = agg["pgcache"] / agg["fastpostgres"] * 100

    style.header(fig, TITLE, SUBTITLE)
    style.panel_title(ax_scan, "the full scan — delivery-bound")
    style.panel_title(ax_agg, "count + two sums — execution-bound")

    handles = draw_panel(
        ax_scan, style, scan_values,
        [fmt_rows(scan_values[0]) + (" rows/s" if style.units_in_labels else ""),
         f"{fmt_rows(scan_values[1])}"
         + (" rows/s\n" if style.units_in_labels else "\n")
         + f"{scan_pct:.0f}% as fast"],
        "rows/s", False, fmt_rows_axis,
    )
    draw_panel(
        ax_agg, style, agg_values,
        [f"{fmt_ms(agg_values[0])} ms",
         f"{fmt_ms(agg_values[1])} ms\n{agg_pct:.0f}% of the time"],
        "ms/read (log)", True, fmt_ms_axis,
    )

    style.legend(fig, handles, SERIES)
    style.note(fig, note_ax, "Read:", READ)
    footer = " · ".join(env.get(k, "?") for k in ("cpu", "postgres", "git"))
    style.footer(fig, footer)

    path = Path(out_dir) / filename
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path
