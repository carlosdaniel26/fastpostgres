#!/usr/bin/env python3
"""Extract the row decode's inner loop from the built client, as llvm-mca input.

The loop worth modeling is the per-column loop inside `decode_tuple`
(codec.cpp): one iteration walks one row, all six kind cases, the varlena
alignment and the births. It is extracted from the built extension - the same
-O2 object the benchmark runs - not from a replica TU, so the ports and the
latency llvm-mca reports are the ports and latency of the code that ships.
(This automates what NOTES.md:17-22 did by hand.)

How the region is chosen: `objdump -dl` carries every instruction's source file
and line, inlined callees included. Without `--line` the outermost loop in the
symbol is modeled - for the built client that is the row loop of
`fp_decode_tuples` (one iteration per row, `decode_tuple` inlined), the loop
NOTES.md counted 334 static instructions for. `--line N` (optionally
with `--source`, default codec.cpp) models the innermost loop whose body carries
an instruction from that line instead; note the loop's own head line can be
attributed to an inlined callee, so an anchor *inside* the body is the reliable
one. `--list` prints every candidate (span, lines, instruction count) so the
choice can be checked rather than trusted.

stdout is the region as AT&T .s, ready for

    llvm-mca -mcpu=znver4 -iterations=100 -bottleneck-analysis

with labels for in-region targets and label-only stubs for the branches that
leave the region. stderr carries the statistics. `just decode-mca` runs both,
with the anchor read out of codec.cpp.
"""
import argparse
import glob
import re
import subprocess
import sys

SYMBOL_RE = re.compile(r"^([0-9a-fA-F]+) <(.+)>:$")
LINE_RE = re.compile(r"^(\S+\.(?:c|cc|cpp|h|hpp|S|s)):(\d+)$")
INSN_RE = re.compile(r"^\s*([0-9a-fA-F]+):\t(.*)$")
ANNOT_RE = re.compile(r"\s*<[^>]*>")
COMMENT_RE = re.compile(r"\s*#.*$")
HEX_RE = re.compile(r"^(?:0x)?[0-9a-fA-F]+$")


def basename(path):
    return path.rsplit("/", 1)[-1]


class Insn:
    __slots__ = ("addr", "text", "line", "source")

    def __init__(self, addr, text, line, source):
        self.addr = addr
        self.text = ANNOT_RE.sub("", text).split("#")[0].strip()
        self.line = line
        self.source = source

    @property
    def mnemonic(self):
        return self.text.split(None, 1)[0] if self.text else ""


def parse(so):
    out = subprocess.run(
        ["objdump", "-d", "-C", "-l", "--no-show-raw-insn", so],
        check=True, text=True, capture_output=True,
    ).stdout
    syms, current, line, source = {}, None, None, None
    for raw in out.splitlines():
        if not raw.strip():
            continue
        if m := SYMBOL_RE.match(raw):
            current = syms.setdefault(m.group(2), [])
            line = source = None
        elif m := LINE_RE.match(raw):
            source, line = basename(m.group(1)), int(m.group(2))
        elif (m := INSN_RE.match(raw)) and current is not None:
            current.append(Insn(int(m.group(1), 16), m.group(2), line, source))
    return syms


def pick_symbol(syms, name):
    """The symbol to look in. `decode_tuple` must not match `fp_decode_tuples`
    by substring, and the `[clone .cold]` of a function is only its error path:
    prefer a whole-function-name match, and a warm clone over a cold one."""
    if name in syms:
        return name
    hits = [n for n in syms if name in n]
    if not hits:
        return None
    whole = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")

    def rank(candidate):
        return (bool(whole.search(candidate)), ".cold" not in candidate)

    return max(hits, key=rank)


def target_of(ins):
    """The absolute address a jump/call goes to, or None (indirect, register)."""
    parts = ins.text.split(None, 1)
    if len(parts) != 2:
        return None
    last = parts[1].rsplit(",", 1)[-1].strip()
    if not HEX_RE.match(last):
        return None
    return int(last, 16)


def is_branch(ins):
    return ins.mnemonic.startswith("j") or ins.mnemonic.startswith("loop")


def candidates(insns):
    """(head index, back-edge index) for every loop in the function."""
    by_addr = {ins.addr: i for i, ins in enumerate(insns)}
    found = []
    for i, ins in enumerate(insns):
        if not is_branch(ins):
            continue
        tgt = target_of(ins)
        if tgt is not None and by_addr.get(tgt, i) < i:
            found.append((by_addr[tgt], i))
    return found


def lines_of(insns, j, k, source):
    """The source lines of [j, k] in `source`, plus the count from elsewhere
    (inlined code from other files), so the audit says what is where."""
    here = [ins.line for ins in insns[j : k + 1]
            if ins.source == source and ins.line is not None]
    other = sum(1 for ins in insns[j : k + 1] if ins.source != source)
    text = f"{min(here)}-{max(here)}" if here else f"no {source} lines"
    return f"{text}{f', {other} from headers' if other else ''}"


def render(ins, labels):
    """The instruction as llvm-mca input: a branch/call target becomes the label
    the region defines (.L<addr>) or a label-only stub (.Lout_<addr>)."""
    mnemonic = ins.mnemonic
    if mnemonic.startswith(("j", "loop", "call")) and ins.text != mnemonic:
        operands = ins.text[len(mnemonic) :].strip()
        head, comma, _ = operands.rpartition(",")
        tgt = target_of(ins)
        if tgt is not None:
            label = f".L{tgt:x}" if tgt in labels else f".Lout_{tgt:x}"
            operands = f"{head}{comma} {label}"
        return f"{mnemonic}\t{operands}"
    return ins.text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--so", default="python/fastpostgres/_client*.so",
                    help="the built extension (glob ok; default %(default)s)")
    ap.add_argument("--symbol", default="decode_tuple", help="function to look in")
    ap.add_argument("--fallback", default="fp_decode_tuples",
                    help="comma-separated symbols to try when --symbol is not there"
                         " (it is inlined into its only caller, or renamed)")
    ap.add_argument("--line", type=int, default=None,
                    help="a source line inside the loop to model"
                         " (default: the outermost loop in the symbol)")
    ap.add_argument("--source", default="codec.cpp", help="the anchor's file")
    ap.add_argument("--list", action="store_true", help="print every candidate loop and exit")
    ap.add_argument("--no-asm", action="store_true", help="statistics only, no .s on stdout")
    args = ap.parse_args()

    matches = glob.glob(args.so)
    if len(matches) != 1:
        sys.exit(f"loop-asm: --so {args.so!r} matched {len(matches)} files (need exactly one)")
    so = matches[0]
    syms = parse(so)

    chosen = pick_symbol(syms, args.symbol)
    if chosen is None:
        for fallback in args.fallback.split(","):
            chosen = pick_symbol(syms, fallback)
            if chosen is not None:
                print(f"loop-asm: {args.symbol!r} is not a symbol (inlined into its caller?);"
                      f" looking in {chosen}", file=sys.stderr)
                break
    if chosen is None:
        sys.exit(f"loop-asm: no symbol matching {args.symbol!r} in {so}")
    if chosen != args.symbol:
        print(f"loop-asm: symbol: {chosen}", file=sys.stderr)
    args.symbol, insns = chosen, syms[chosen]

    if not insns:
        sys.exit(f"loop-asm: no instructions found for {args.symbol!r} in {so}")

    cands = candidates(insns)
    if not cands:
        sys.exit(f"loop-asm: no loop found in {args.symbol} ({len(insns)} instructions)")

    if args.list:
        for j, k in sorted(cands, key=lambda c: c[1] - c[0], reverse=True):
            print(f"loop-asm: 0x{insns[j].addr:x}-0x{insns[k].addr:x}  {k - j + 1:4d} insns  "
                  f"lines {lines_of(insns, j, k, args.source)}", file=sys.stderr)
        return 0

    anchor = None
    if args.line is not None:
        anchor = [i for i, ins in enumerate(insns)
                  if ins.source == args.source and ins.line == args.line]
        if not anchor:
            print(f"loop-asm: WARNING: no instruction at {args.source}:{args.line} - "
                  f"the anchor is not in this function (--list shows the loops)",
                  file=sys.stderr)
    containing = [(j, k) for j, k in cands
                  if anchor and any(j <= i <= k for i in anchor)]
    if containing:
        j, k = min(containing, key=lambda c: c[1] - c[0])
    else:
        j, k = max(cands, key=lambda c: c[1] - c[0])
        if anchor is None:
            print("loop-asm: region: the outermost loop in the symbol"
                  " (--line N models another; --list shows the candidates)", file=sys.stderr)
        else:
            print("loop-asm: WARNING: no loop contains the anchor, using the widest span",
                  file=sys.stderr)

    region = insns[j : k + 1]
    addrs = {ins.addr for ins in region}
    label_at, stubs, calls = set(), set(), 0
    for ins in region:
        if ins.mnemonic.startswith("call"):
            calls += 1
        if not ins.mnemonic.startswith(("j", "loop", "call")):
            continue
        tgt = target_of(ins)
        if tgt is None:
            continue
        if tgt in addrs and ins.mnemonic.startswith(("j", "loop")):
            label_at.add(tgt)
        else:
            stubs.add(tgt)
    out_of_region = {t for t in stubs if t not in addrs}

    print(f"loop-asm: object: {so}", file=sys.stderr)
    print(f"loop-asm: loop: 0x{region[0].addr:x}-0x{region[-1].addr:x}  "
          f"{len(region)} instructions  lines {lines_of(insns, j, k, args.source)}"
          f"  back edge: {region[-1].mnemonic}", file=sys.stderr)
    if args.line is not None:
        print(f"loop-asm: anchor: {args.source}:{args.line}"
              f"{' (innermost loop containing it)' if containing else ''}", file=sys.stderr)
    print(f"loop-asm: {calls} calls in the region: the callee's cost is not modeled",
          file=sys.stderr)
    print(f"loop-asm: {len(out_of_region)} branches leave the region (label-only stubs)",
          file=sys.stderr)

    if not args.no_asm:
        for ins in region:
            if ins.addr in label_at:
                print(f".L{ins.addr:x}:")
            print(f"\t{render(ins, label_at)}")
        if stubs:
            print()
            print("# --- paths out of the region (not modeled): labels only,")
            print("# --- so the branches and calls above assemble")
            for t in sorted(stubs):
                print(f".Lout_{t:x}:")
    return 0


if __name__ == "__main__":
    sys.exit(main())
