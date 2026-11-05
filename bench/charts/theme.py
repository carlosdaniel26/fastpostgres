#!/usr/bin/env python3
"""The house chart theme: spines, grid, legend and caption, in one place.

`chart-example.png`, in this directory, is the style reference: thin black axes
with no top or right spine, a boxed legend inside the plot, and a small gray
caption under the figure. The grid is not part of the look: the axis places the
bar and the bar carries its own value, so grid lines only add ink. And the scales
are read, not counted: a handful of major ticks from a 1-2-5 progression, and on
a logarithmic axis the decades plus a 3 (1, 3, 10, 30, ...). The type is not the
exemplar's Computer Modern: it is matplotlib's default sans (DejaVu Sans), the
face these charts have always been drawn in. Every chart script imports this
module and takes its defaults from here, so the look is decided once instead of
restated per script.

Colors are deliberately not part of the theme: each chart keeps its own palette
and its light-before/dark-after convention, because there the color carries the
reading.

    import theme

    theme.declutter(ax)                            # spines, ticks
    theme.ticks(ax)                                # a few major ticks
    theme.legend(ax)                               # boxed, top left
    theme.footer(fig, "cpu: ... · git abc123")     # the caption
    fig.savefig(path)                              # dpi and ground already set
"""
import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402
from matplotlib.figure import Figure  # noqa: E402

# matplotlib's bundled default: what these charts rendered in before the theme
# existed, and what they render in again.
FONT = "DejaVu Sans"

CAPTION_COLOR = "#888888"


def apply():
    """The matplotlib defaults the theme is made of; runs on import."""
    plt.rcParams.update({
        "font.family": FONT,
        "font.size": 9.5,
        "axes.labelsize": 10,
        "axes.titlesize": 10.5,
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "legend.fontsize": 8.5,
        "figure.titlesize": 12.5,
        "axes.facecolor": "white",
        "figure.facecolor": "white",
        "savefig.facecolor": "white",
        "savefig.dpi": 150,
        "axes.edgecolor": "black",
        "axes.linewidth": 0.8,
        "axes.axisbelow": True,
        "xtick.color": "black",
        "ytick.color": "black",
        "xtick.major.width": 0.8,
        "ytick.major.width": 0.8,
        "xtick.major.size": 3.5,
        "ytick.major.size": 3.5,
    })


def declutter(ax):
    """Left and bottom spines only, ticks black, no grid."""
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("black")
        ax.spines[side].set_linewidth(0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors="black")
    return ax


def ticks(ax, axis="y", n=5, log=False):
    """A handful of major ticks: the scale is read, not counted.

    A linear axis gets at most `n` (three to five, in practice) ticks from a
    1-2-5 progression; a logarithmic
    one gets its decades plus a 3 (1, 3, 10, 30, ...), with the minor labels off.
    Only the value axis is touched, so a categorical axis keeps its own ticks.
    """
    target = ax.yaxis if axis == "y" else ax.xaxis
    if log:
        target.set_major_locator(mticker.LogLocator(base=10.0, subs=(1.0, 3.0), numticks=12))
        target.set_minor_formatter(mticker.NullFormatter())
    else:
        target.set_major_locator(mticker.MaxNLocator(nbins=n, steps=[1, 2, 5, 10]))
    return ax


def legend(parent, loc="upper left", **kw):
    """The boxed legend, framed thin and black - when it sits on the plot.

    The exemplar frames its legend because that legend is inside the plot, over
    the white axes. A legend that lives in the figure's own white space - the
    row between the title and the panels - is left frameless the way these
    charts had it, since the same box there reads as a stray rectangle. Pass
    the axes for the box, the figure for none; `frameon` overrides either.
    """
    boxed = not isinstance(parent, Figure)
    kw.setdefault("frameon", boxed)
    kw.setdefault("fancybox", False)
    kw.setdefault("framealpha", 1.0)
    kw.setdefault("facecolor", "white")
    kw.setdefault("edgecolor", "black")
    handle = parent.legend(loc=loc, **kw)
    handle.get_frame().set_linewidth(0.8)
    return handle


def footer(fig, text, y=0.02, **kw):
    """The caption under the figure: small, gray, serif, one provenance line."""
    kw.setdefault("ha", "center")
    kw.setdefault("va", "bottom")
    kw.setdefault("fontsize", 8)
    kw.setdefault("color", CAPTION_COLOR)
    kw.setdefault("linespacing", 1.5)
    return fig.text(0.5, y, text, **kw)


apply()
