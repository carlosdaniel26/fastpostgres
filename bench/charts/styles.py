#!/usr/bin/env python3
"""Candidate looks for the house charts, kept in one place while we choose.

The charts themselves are data and layout; the look is a `Style`. A style owns
the fonts, the paper, the axis frame, how a bar is filled, and how the header,
the note and the source line are set. `style_lab.py` renders the same figure
through every style below so the comparison is only about the look.

    from styles import STYLES
    style = STYLES["sketch"]()

Adding a style means adding a subclass and putting it in `STYLES`; nothing in
the chart scripts has to change.

`house` is the current look (`theme.py`) and stays the default. The others are
experiments for the `chart-styles` branch.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.patheffects as pe  # noqa: E402
from matplotlib import font_manager  # noqa: E402
from matplotlib.font_manager import FontProperties  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402
from matplotlib.transforms import ScaledTranslation  # noqa: E402

import theme  # noqa: E402

FONT_DIR = Path(__file__).with_name("fonts")


def _register_fonts() -> None:
    """Make the bundled handwriting fonts available to matplotlib."""
    for path in FONT_DIR.glob("*.ttf"):
        font_manager.fontManager.addfont(str(path))


_register_fonts()

HAND = FontProperties(family="Patrick Hand")
BRUSH = FontProperties(family="Caveat Brush")


class Style:
    """One look: palette, type, frame, bars, and the furniture around them."""

    name = "house"
    label = "house (current)"
    paper = "white"
    ink = "black"
    muted = "#888888"
    series = ("#1f77b4", "#e1812c")
    accent = "#e1812c"
    font = ("DejaVu Sans",)
    font_size = 9.5
    title_size = 13.5
    subtitle_size = 9.5
    title_font: FontProperties | None = None
    sketch: tuple[float, float, float] | None = None
    grid: bool = False
    grid_color: str = "#dddddd"
    grid_width: float = 0.6
    axis_width: float = 0.8
    axis_color: str | None = None
    y_axis_visible = True
    color_x_labels = False
    x_bold = False
    show_legend = True
    tick_size = 3.5
    bar_alpha = 1.0
    bar_hatch: str | None = None
    bar_edge: str | None = None
    bar_linewidth = 0.0
    bar_rounding = 0.0
    bar_width = 0.30
    bar_gap = 1.15
    label_size = 8.0
    label_series_color = False
    label_bold = False
    units_in_labels = False
    y_margin = 0.16
    note_face = "none"
    note_edge = "none"
    note_linewidth = 0.0

    # -- global setup -----------------------------------------------------
    def setup(self, fig) -> None:
        matplotlib.rcParams.update({
            "font.family": self.font,
            "font.size": self.font_size,
            "axes.labelsize": self.font_size,
            "axes.titlesize": self.font_size,
            "xtick.labelsize": self.font_size - 0.5,
            "ytick.labelsize": self.font_size - 0.5,
            "legend.fontsize": self.font_size - 1,
            "axes.facecolor": self.paper,
            "figure.facecolor": self.paper,
            "savefig.facecolor": self.paper,
            "axes.edgecolor": self.ink,
            "axes.linewidth": self.axis_width,
            "axes.axisbelow": True,
            "xtick.color": self.ink,
            "ytick.color": self.ink,
            "xtick.major.width": self.axis_width,
            "ytick.major.width": self.axis_width,
            "xtick.major.size": self.tick_size,
            "ytick.major.size": self.tick_size,
            "text.color": self.ink,
            "grid.color": self.grid_color,
            "grid.linewidth": self.grid_width,
            "path.sketch": self.sketch,
        })
        fig.patch.set_facecolor(self.paper)

    # -- axis frame -------------------------------------------------------
    def frame(self, ax) -> None:
        color = self.axis_color or self.ink
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(color)
            ax.spines[side].set_linewidth(self.axis_width)
        ax.tick_params(colors=self.ink, width=self.axis_width, length=self.tick_size)
        if not self.y_axis_visible:
            ax.spines["left"].set_visible(False)
            ax.tick_params(axis="y", which="both", left=False, labelleft=False)
            ax.set_ylabel("")
        if self.grid:
            ax.grid(True, axis="y", color=self.grid_color, linewidth=self.grid_width)
        ax.set_axisbelow(True)

    def x_labels(self, ax, names) -> None:
        """Colour and weight the category labels, when the style wants that."""
        for tick, color in zip(ax.get_xticklabels(), self.series):
            tick.set_color(color if self.color_x_labels else self.ink)
            if self.x_bold:
                tick.set_fontweight("bold")

    # -- bars -------------------------------------------------------------
    def bar(self, ax, x, height, width, index):
        kw = {
            "color": self.series[index],
            "width": width,
            "alpha": self.bar_alpha,
            "zorder": 3,
        }
        if self.bar_hatch:
            kw["hatch"] = self.bar_hatch
            kw["edgecolor"] = self.bar_edge or self.series[index]
            kw["linewidth"] = self.bar_linewidth
        if self.bar_rounding:
            kw["linewidth"] = self.bar_linewidth or 0.0
            kw["edgecolor"] = self.bar_edge or self.series[index]
        bars = ax.bar([x], [height], **kw)
        return bars

    def bar_label(self, ax, bars, text, index=0):
        ax.annotate(
            text,
            (bars[0].get_x() + bars[0].get_width() / 2, bars[0].get_height()),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=self.label_size,
            color=self.series[index] if self.label_series_color else self.ink,
            fontweight="bold" if self.label_bold else "normal",
        )

    # -- figure furniture -------------------------------------------------
    def header(self, fig, title, subtitle) -> None:
        kw = {"fontproperties": self.title_font} if self.title_font else {"fontweight": "bold"}
        fig.text(0.065, 0.955, title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, **kw)
        fig.text(0.065, 0.885, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted, style="italic")

    def panel_title(self, ax, text) -> None:
        ax.set_title(text, loc="left", fontsize=self.font_size, color=self.ink,
                     fontproperties=self.title_font if self.sketch else None,
                     pad=8)

    def legend(self, fig, handles, labels) -> None:
        if not self.show_legend:
            return
        fig.legend(handles, labels, loc="upper right", bbox_to_anchor=(0.95, 0.90),
                   frameon=False, ncol=1, labelcolor=self.ink)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        if self.note_face != "none":
            ax.add_patch(Rectangle((0.0, 0.02), 1.0, 0.92, transform=ax.transAxes,
                                   facecolor=self.note_face,
                                   edgecolor=self.note_edge or self.ink,
                                   linewidth=self.note_linewidth, zorder=2))
        ax.text(0.025, 0.72, lead, transform=ax.transAxes, ha="left", va="top",
                fontsize=self.font_size, color=self.accent,
                fontproperties=self.title_font if self.sketch else None,
                fontweight=None if self.sketch else "bold", zorder=3)
        ax.text(0.10, 0.72, text, transform=ax.transAxes, ha="left", va="top",
                fontsize=self.font_size, color=self.ink,
                fontproperties=self.title_font if self.sketch else None, zorder=3)

    def footer(self, fig, text) -> None:
        fig.text(0.065, 0.015, text, ha="left", va="bottom",
                 fontsize=self.font_size - 1.5, color=self.muted)


class House(Style):
    """The look the charts have today, from `theme.py`."""

    name = "house"
    label = "house (current)"

    def setup(self, fig) -> None:
        theme.apply()
        matplotlib.rcParams["path.sketch"] = None
        fig.patch.set_facecolor("white")

    def frame(self, ax) -> None:
        theme.declutter(ax)
        theme.ticks(ax, log=ax.get_yscale() == "log")

    def header(self, fig, title, subtitle) -> None:
        fig.text(0.5, 0.965, title, ha="center", va="top",
                 fontsize=13.5, color="black", fontweight="bold")
        fig.text(0.5, 0.905, subtitle, ha="center", va="top",
                 fontsize=9.5, color="#555555")

    def panel_title(self, ax, text) -> None:
        ax.set_title(text, loc="center", fontsize=9.5, color="black", pad=8)

    def legend(self, fig, handles, labels) -> None:
        theme.legend(fig, loc="upper center", bbox_to_anchor=(0.5, 0.895),
                     ncol=2, handles=handles, labels=labels, fontsize=9)

    def bar_label(self, ax, bars, text, index=0) -> None:
        ax.bar_label(bars, labels=[text], fontsize=6.8, padding=2)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        ax.text(0.0, 0.58, f"{lead} {text}", transform=ax.transAxes,
                ha="left", va="center", fontsize=9, color="#555555")

    def footer(self, fig, text) -> None:
        theme.footer(fig, text, y=0.012)


class Sketch(Style):
    """The hand-drawn reference: warm paper, ink strokes, marker type, hatching."""

    name = "sketch"
    label = "sketch (hand-drawn)"
    paper = "#fdfbf3"
    ink = "#262626"
    muted = "#6b6b64"
    series = ("#4c8bf5", "#e05252")
    accent = "#e07a1f"
    font = ["Patrick Hand", "DejaVu Sans"]
    font_size = 11.5
    title_size = 27
    subtitle_size = 12.5
    title_font = BRUSH
    sketch = (1.4, 110.0, 2.0)
    axis_width = 1.5
    tick_size = 0.0
    bar_alpha = 0.30
    bar_hatch = "///"
    bar_edge = "#262626"
    bar_linewidth = 1.4
    label_size = 12.5
    units_in_labels = True
    y_margin = 0.30
    note_face = "#fbf1b8"
    note_edge = "#262626"
    note_linewidth = 1.6

    def setup(self, fig) -> None:
        super().setup(fig)
        # dotted paper, drawn behind everything
        bg = fig.add_axes((0, 0, 1, 1), zorder=-10)
        bg.set_axis_off()
        bg.set_facecolor(self.paper)
        bg.set_xlim(0, 1)
        bg.set_ylim(0, 1)
        xs = [x / 44 for x in range(45)]
        ys = [y / 30 for y in range(31)]
        bg.scatter([x for x in xs for _ in ys], [y for _ in xs for y in ys],
                   s=0.8, color="#ded8c8", marker="o", linewidths=0, zorder=-9)

    def frame(self, ax) -> None:
        super().frame(ax)
        ax.tick_params(axis="both", length=0)
        # the reference has no value axis: the bar carries its number instead
        ax.spines["left"].set_visible(False)
        ax.tick_params(axis="y", which="both", length=0, labelleft=False)
        ax.tick_params(axis="x", pad=7)
        ax.set_ylabel("")

    def bar(self, ax, x, height, width, index):
        # tinted block, ink outline, then the reference's colored diagonal hatch
        base = ax.bar([x], [height], width=width, facecolor=self.series[index],
                      alpha=0.22, edgecolor=self.ink, linewidth=1.4, zorder=3)
        ax.bar([x], [height], width=width, facecolor="none",
               edgecolor=self.series[index], linewidth=0, hatch="///", zorder=4)
        return base

    def bar_label(self, ax, bars, text, index=0) -> None:
        ax.annotate(
            text,
            (bars[0].get_x() + bars[0].get_width() / 2, bars[0].get_height()),
            xytext=(0, 5),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=self.label_size,
            color=self.ink,
            fontproperties=HAND,
        )

    def header(self, fig, title, subtitle) -> None:
        fig.text(0.06, 0.965, title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, fontproperties=self.title_font)
        fig.text(0.06, 0.845, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted,
                 fontproperties=HAND, style="italic")
        line = plt.Line2D((0.06, 0.42), (0.872, 0.872), transform=fig.transFigure,
                          color="#e0702a", linewidth=3.2, solid_capstyle="round",
                          path_effects=None)
        fig.add_artist(line)

    def panel_title(self, ax, text) -> None:
        ax.set_title(text, loc="left", fontsize=self.font_size + 1.5,
                     color=self.ink, fontproperties=HAND, pad=10)

    def legend(self, fig, handles, labels) -> None:
        leg = fig.legend(handles, labels, loc="upper right", bbox_to_anchor=(0.95, 0.87),
                         frameon=True, fancybox=False, framealpha=1, ncol=1,
                         facecolor=self.paper, edgecolor=self.ink, labelcolor=self.ink,
                         prop=FontProperties(family="Patrick Hand", size=11))
        leg.get_frame().set_linewidth(1.2)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        ax.add_patch(Rectangle((0.0, 0.02), 1.0, 0.92, transform=ax.transAxes,
                               facecolor=self.note_face, edgecolor=self.note_edge,
                               linewidth=self.note_linewidth, zorder=2))
        ax.text(0.025, 0.68, lead, transform=ax.transAxes, ha="left", va="center",
                fontsize=self.font_size + 0.5, color=self.accent,
                fontproperties=HAND, zorder=3)
        ax.text(0.145, 0.68, text, transform=ax.transAxes, ha="left", va="center",
                fontsize=self.font_size, color=self.ink,
                fontproperties=HAND, zorder=3)

    def footer(self, fig, text) -> None:
        fig.text(0.06, 0.018, text, ha="left", va="bottom",
                 fontsize=9.5, color=self.muted, fontproperties=HAND, style="italic")


class Editorial(Style):
    """Magazine page: serif type, warm white, hairline rules, muted print colors."""

    name = "editorial"
    label = "editorial (print)"
    paper = "#faf8f2"
    ink = "#1d1d1b"
    muted = "#777268"
    series = ("#2f5d62", "#b3452f")
    accent = "#b3452f"
    font = ["Noto Serif", "DejaVu Serif"]
    font_size = 10.5
    title_size = 20
    subtitle_size = 10.5
    axis_width = 0.7
    bar_alpha = 1.0
    bar_linewidth = 0.0
    label_size = 10
    note_face = "#f3efe5"
    note_edge = "none"
    note_linewidth = 0.0

    def setup(self, fig) -> None:
        super().setup(fig)
        fig.patch.set_facecolor(self.paper)

    def frame(self, ax) -> None:
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(self.ink)
        ax.spines["bottom"].set_linewidth(self.axis_width)
        ax.tick_params(axis="y", which="both", length=0, labelleft=False)
        ax.tick_params(axis="x", which="both", length=0, pad=6)

    def header(self, fig, title, subtitle) -> None:
        fig.text(0.065, 0.96, title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, style="italic")
        fig.text(0.065, 0.875, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted)
        fig.add_artist(plt.Line2D((0.065, 0.935), (0.845, 0.845),
                                  transform=fig.transFigure, color=self.ink, linewidth=0.6))

    def panel_title(self, ax, text) -> None:
        ax.set_title(text, loc="left", fontsize=self.font_size - 0.5,
                     color=self.muted, style="italic", pad=10)

    def legend(self, fig, handles, labels) -> None:
        fig.legend(handles, labels, loc="upper right", bbox_to_anchor=(0.94, 0.87),
                   frameon=False, ncol=1, labelcolor=self.ink)

    def note(self, fig, ax, lead, text) -> None:
        super().note(fig, ax, lead, text)

    def footer(self, fig, text) -> None:
        fig.text(0.065, 0.016, text, ha="left", va="bottom",
                 fontsize=8.5, color=self.muted, style="italic")


class Mono(Style):
    """Terminal / lab notebook: dark ground, monospace, cyan and amber traces."""

    name = "mono"
    label = "mono (terminal)"
    paper = "#0e1116"
    ink = "#e6e9ef"
    muted = "#8a93a3"
    series = ("#6fd3c7", "#e0a458")
    accent = "#e0a458"
    font = ["DejaVu Sans Mono", "monospace"]
    font_size = 9.5
    title_size = 14
    subtitle_size = 9.5
    grid = True
    grid_color = "#242a33"
    axis_width = 0.8
    bar_alpha = 0.9
    bar_edge = "#0e1116"
    bar_linewidth = 0.0
    label_size = 8.5
    note_face = "#141922"
    note_edge = "#2c3442"
    note_linewidth = 1.0

    def setup(self, fig) -> None:
        super().setup(fig)
        fig.patch.set_facecolor(self.paper)

    def header(self, fig, title, subtitle) -> None:
        fig.text(0.065, 0.955, "▍ " + title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, fontweight="bold")
        fig.text(0.065, 0.885, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted)

    def panel_title(self, ax, text) -> None:
        ax.set_title(text, loc="left", fontsize=self.font_size - 0.5,
                     color=self.muted, pad=8)

    def frame(self, ax) -> None:
        super().frame(ax)
        ax.yaxis.labelpad = 6

    def legend(self, fig, handles, labels) -> None:
        fig.legend(handles, labels, loc="upper right", bbox_to_anchor=(0.95, 0.87),
                   frameon=False, ncol=1, labelcolor=self.ink)

    def note(self, fig, ax, lead, text) -> None:
        super().note(fig, ax, lead, text)

    def footer(self, fig, text) -> None:
        fig.text(0.065, 0.016, text, ha="left", va="bottom",
                 fontsize=8, color=self.muted)


class Slide(Style):
    """Keynote slide: white space, one accent, huge direct numbers, no axis."""

    name = "slide"
    label = "slide (keynote)"
    paper = "#ffffff"
    ink = "#0f172a"
    muted = "#64748b"
    series = ("#2563eb", "#f97316")
    accent = "#2563eb"
    font = ("DejaVu Sans",)
    font_size = 11
    title_size = 28
    subtitle_size = 13
    axis_width = 1.0
    axis_color = "#e2e8f0"
    y_axis_visible = False
    tick_size = 0
    label_size = 16.5
    label_bold = True
    label_series_color = True
    units_in_labels = True
    y_margin = 0.36
    bar_gap = 1.85
    color_x_labels = True
    x_bold = True
    show_legend = False

    def header(self, fig, title, subtitle) -> None:
        fig.add_artist(plt.Line2D((0.06, 0.94), (0.985, 0.985), transform=fig.transFigure,
                                  color=self.accent, linewidth=4, solid_capstyle="butt"))
        fig.text(0.06, 0.945, title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, fontweight="bold")
        fig.text(0.06, 0.87, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted)

    def panel_title(self, ax, text) -> None:
        ax.set_title(text.upper(), loc="left", fontsize=11.5, color=self.muted,
                     fontweight="bold", pad=12)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        ax.add_patch(Rectangle((0.0, 0.16), 0.004, 0.68, transform=ax.transAxes,
                               facecolor=self.accent, edgecolor="none"))
        ax.text(0.018, 0.82, "TAKEAWAY", transform=ax.transAxes, ha="left", va="top",
                fontsize=9, color=self.accent, fontweight="bold")
        ax.text(0.018, 0.42, text, transform=ax.transAxes, ha="left", va="center",
                fontsize=14.5, color=self.ink, fontweight="bold")

    def footer(self, fig, text) -> None:
        fig.text(0.06, 0.018, text, ha="left", va="bottom",
                 fontsize=8.5, color=self.muted)


class Spotlight(Style):
    """Conference slide: dark ground, glowing bars, large white type."""

    name = "spotlight"
    label = "spotlight (dark slide)"
    paper = "#0b1020"
    ink = "#f8fafc"
    muted = "#94a3b8"
    series = ("#4f8cff", "#ff6b6b")
    accent = "#7dd3fc"
    font = ("DejaVu Sans",)
    font_size = 11
    title_size = 28
    subtitle_size = 13
    axis_width = 1.0
    axis_color = "#1e293b"
    y_axis_visible = False
    tick_size = 0
    label_size = 16.5
    label_bold = True
    label_series_color = True
    units_in_labels = True
    y_margin = 0.36
    bar_gap = 1.85
    color_x_labels = True
    x_bold = True
    show_legend = False
    note_face = "#10182b"
    note_edge = "#1e293b"
    note_linewidth = 1.0

    def setup(self, fig) -> None:
        super().setup(fig)
        fig.patch.set_facecolor(self.paper)

    def bar(self, ax, x, height, width, index):
        bars = super().bar(ax, x, height, width, index)
        glow = pe.withStroke(linewidth=12, foreground=self.series[index], alpha=0.22)
        for rect in bars:
            rect.set_path_effects([glow])
        return bars

    def header(self, fig, title, subtitle) -> None:
        fig.add_artist(plt.Line2D((0.06, 0.94), (0.985, 0.985), transform=fig.transFigure,
                                  color=self.accent, linewidth=3, alpha=0.7))
        fig.text(0.06, 0.945, title, ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, fontweight="bold")
        fig.text(0.06, 0.87, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted)

    def panel_title(self, ax, text) -> None:
        ax.set_title(text.upper(), loc="left", fontsize=11.5, color=self.muted,
                     fontweight="bold", pad=12)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        ax.add_patch(Rectangle((0.0, 0.06), 1.0, 0.86, transform=ax.transAxes,
                               facecolor=self.note_face, edgecolor=self.note_edge,
                               linewidth=self.note_linewidth))
        ax.text(0.018, 0.76, "TAKEAWAY", transform=ax.transAxes, ha="left", va="top",
                fontsize=9, color=self.accent, fontweight="bold")
        ax.text(0.018, 0.36, text, transform=ax.transAxes, ha="left", va="center",
                fontsize=14.5, color=self.ink, fontweight="bold")

    def footer(self, fig, text) -> None:
        fig.text(0.06, 0.018, text, ha="left", va="bottom",
                 fontsize=8.5, color="#64748b")


class Brutal(Style):
    """Neo-brutalist slide: flat blocks, thick ink outlines, hard offset shadows."""

    name = "brutal"
    label = "brutal (neo-brutalist)"
    paper = "#fdf6e3"
    ink = "#111111"
    muted = "#57534e"
    series = ("#2563eb", "#ef4444")
    accent = "#111111"
    font = ("DejaVu Sans",)
    font_size = 11
    title_size = 27
    subtitle_size = 12.5
    axis_width = 2.0
    y_axis_visible = False
    tick_size = 0
    label_size = 16.5
    label_bold = True
    units_in_labels = True
    y_margin = 0.38
    bar_gap = 1.85
    color_x_labels = True
    x_bold = True
    show_legend = False
    note_face = "#fde68a"
    note_edge = "#111111"
    note_linewidth = 2.2

    def setup(self, fig) -> None:
        super().setup(fig)
        fig.patch.set_facecolor(self.paper)

    def bar(self, ax, x, height, width, index):
        base = ax.bar([x], [height], width=width, color=self.series[index],
                      edgecolor=self.ink, linewidth=2.4, zorder=3)
        # the hard shadow is offset in points, so the axes transform can change
        # (the aggregate panel is logarithmic) without stretching it
        bottom = ax.get_ylim()[0] if ax.get_yscale() == "log" else 0.0
        offset = ScaledTranslation(4 / 72, -4 / 72, ax.figure.dpi_scale_trans)
        shadow = Rectangle((x - width / 2, bottom), width, height - bottom,
                           transform=ax.transData + offset, facecolor=self.ink,
                           edgecolor="none", zorder=2.5, clip_on=True)
        ax.add_patch(shadow)
        return base

    def header(self, fig, title, subtitle) -> None:
        fig.text(0.06, 0.945, title.upper(), ha="left", va="top",
                 fontsize=self.title_size, color=self.ink, fontweight="bold")
        fig.add_artist(plt.Line2D((0.06, 0.34), (0.875, 0.875), transform=fig.transFigure,
                                  color=self.ink, linewidth=4))
        fig.text(0.06, 0.825, subtitle, ha="left", va="top",
                 fontsize=self.subtitle_size, color=self.muted, fontweight="bold")

    def panel_title(self, ax, text) -> None:
        ax.set_title(text.upper(), loc="left", fontsize=12, color=self.ink,
                     fontweight="bold", pad=12)

    def note(self, fig, ax, lead, text) -> None:
        ax.set_axis_off()
        ax.add_patch(Rectangle((0.006, -0.02), 1.0, 0.9, transform=ax.transAxes,
                               facecolor=self.ink, edgecolor="none"))
        ax.add_patch(Rectangle((0.0, 0.04), 1.0, 0.9, transform=ax.transAxes,
                               facecolor=self.note_face, edgecolor=self.note_edge,
                               linewidth=self.note_linewidth))
        ax.text(0.018, 0.74, "TAKEAWAY", transform=ax.transAxes, ha="left", va="top",
                fontsize=10, color=self.ink, fontweight="bold")
        ax.text(0.018, 0.36, text, transform=ax.transAxes, ha="left", va="center",
                fontsize=14, color=self.ink, fontweight="bold")

    def footer(self, fig, text) -> None:
        fig.text(0.06, 0.018, text, ha="left", va="bottom",
                 fontsize=9, color=self.muted, fontweight="bold")


STYLES = {style.name: style for style in (
    House(), Sketch(), Editorial(), Mono(), Slide(), Spotlight(), Brutal(),
)}
GROUP_PRESENTATION = ("house", "sketch", "slide", "spotlight", "brutal")
GROUP_REPOSITORY = ("house", "editorial", "mono")
GROUPS = {
    "presentation": GROUP_PRESENTATION,
    "repository": GROUP_REPOSITORY,
    "all": tuple(dict.fromkeys((*GROUP_PRESENTATION, *GROUP_REPOSITORY))),
}
ORDER = GROUPS["all"]

# The styles picked for each output. The lab keeps the whole set for reference;
# these two are what the deck and the repository should actually use.
CHOSEN = {
    "presentation": "brutal",
    "repository": "mono",
}
