#!/usr/bin/env python3
"""Render one real figure through every candidate style, side by side.

The data is the committed fast-postgres vs PgCache run at 1M rows, so the lab
compares looks, not numbers. Each style writes its own PNG plus a `contact.png`
that stacks them; `--style` renders one, `--out` chooses where the files go.

    just charts styles                       # all styles -> docs/assets/style-lab/
    just charts styles --group presentation  # the slide-friendly set
    just charts styles --style sketch        # one style
    just charts styles --chosen presentation # the style picked for that output
    just charts styles --out /tmp/lab        # somewhere else
"""
import argparse
import sys
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required (pip install matplotlib)")

import cache  # noqa: E402  (the log parser the real chart uses)
import cache_figure  # noqa: E402
import styles  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = ROOT / "bench" / "results" / "fastpostgres-vs-pgcache.log"
DEFAULT_OUT = ROOT / "docs" / "assets" / "style-lab"


def load(log):
    """(the largest size's data, env) for the comparison."""
    text = Path(log).read_text()
    data, _fills, env = cache.parse(text)
    if not data:
        sys.exit(f"{log}: no rows found -- run `just pgcache-bench` first")
    return data[max(data)], env


def contact_sheet(paths, out_dir, name="contact.png"):
    """Stack the renders into one tall image for a single-glance comparison."""
    from matplotlib.image import imread

    images = [imread(p) for p in paths]
    height = sum(im.shape[0] for im in images)
    width = max(im.shape[1] for im in images)
    fig = plt.figure(figsize=(width / 100, height / 100), dpi=100)
    ax = fig.add_axes((0, 0, 1, 1))
    ax.set_axis_off()
    y = height
    for im in images:
        y -= im.shape[0]
        ax.imshow(im, extent=(0, im.shape[1], y, y + im.shape[0]), aspect="auto")
    ax.set_xlim(0, width)
    ax.set_ylim(0, height)
    path = out_dir / name
    fig.savefig(path, dpi=100)
    plt.close(fig)
    return path


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--style", choices=(*styles.ORDER, "all"), default="all",
                    help="which style to render (default: all)")
    ap.add_argument("--group", choices=tuple(styles.GROUPS), default="all",
                    help="a named set of styles (default: all)")
    ap.add_argument("--chosen", choices=tuple(styles.CHOSEN),
                    help="render the style selected for an output")
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    data, env = load(args.log)

    if args.chosen:
        names = [styles.CHOSEN[args.chosen]]
    elif args.style != "all":
        names = [args.style]
    else:
        names = list(styles.GROUPS[args.group])

    paths = []
    for name in names:
        path = cache_figure.render(name, data, env, args.out, f"{name}.png")
        paths.append(path)
        print(f"wrote {path}")
    if len(paths) > 1:
        contact = "contact.png" if args.group == "all" else f"contact-{args.group}.png"
        print(f"wrote {contact_sheet(paths, args.out, contact)}")


if __name__ == "__main__":
    main()
