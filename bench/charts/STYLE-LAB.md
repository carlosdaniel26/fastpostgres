# Chart style lab

The committed charts are drawn by `theme.py` and read as default matplotlib:
thin axes, DejaVu Sans, value labels. This branch is where we try other looks,
using the same figure and the same committed numbers so the comparison is only
about the style.

```
just charts styles                     # all candidates -> docs/assets/style-lab/
just charts styles --group presentation # only the slide-friendly set
just charts styles --group repository   # only the docs-friendly set
just charts styles --style sketch       # one candidate
just charts styles --chosen presentation # just the picked style (brutal)
just charts styles --chosen repository   # just the picked style (mono)
just charts styles --out /tmp/lab       # somewhere else
```

`contact.png` stacks the candidates into one image; a group writes
`contact-<group>.png`.

## Chosen

| output | style | command |
| --- | --- | --- |
| repository | `mono` | `just charts cache` |
| presentation | `brutal` | `just presentation` |

`cache.py` now renders through `styles.py`: the repository default is the mono
look, and `just presentation` re-renders the same log in brutal into
`presentation/assets/`. The other chart scripts still use `theme.py`; they move
over one at a time, and each one added to `bench/dispatch/presentation.sh`
appears in the deck set.

The full set stays in the lab; `--chosen <output>` renders only the selected
look, so the deck and the docs can be regenerated without the exploration noise.

## Candidates

| style | group | character |
| --- | --- | --- |
| `house` | both | the current look, as the baseline |
| `sketch` | presentation | hand-drawn reference: warm dotted paper, ink strokes, marker title, tinted diagonal-hatch bars, no value axis, boxed note |
| `slide` | presentation | keynote: white space, one accent, huge direct numbers, no axis |
| `spotlight` | presentation | conference slide: dark ground, glowing bars, large white type |
| `brutal` | presentation | neo-brutalist: flat blocks, thick ink outlines, hard offset shadows |
| `editorial` | repository | print magazine: serif type, warm white, hairline rule, muted teal/terracotta, direct labels |
| `mono` | repository | terminal / lab notebook: dark ground, monospace, cyan and amber bars, faint grid |

`sketch` is the closest to the hand-drawn example that started this branch. It
drops the value axis on purpose: the bar carries its number, the way the
reference does, so the plot reads as an illustration rather than a screenshot
of matplotlib.

## Files

- `styles.py` — one `Style` subclass per candidate. A style owns the fonts, the
  paper, the axis frame, the bar fill, and the header/note/footer furniture.
- `style_lab.py` — renders the cache figure through every style; data comes from
  the same log the cache chart reads.
- `fonts/` — bundled OFL handwriting fonts: Patrick Hand for labels, Caveat
  Brush for the sketch title. Licenses sit next to them.

To add a candidate, subclass `Style`, override what differs, and append it to
`STYLES` / `ORDER` in `styles.py`. The chart scripts do not change: once a
direction is picked, its settings move into `theme.py` and the lab stays as a
place for the next experiment.

Production charts still import `theme.py`. Nothing on this branch changes how
`just charts cache` or the other committed figures render.
