#!/usr/bin/env python3
"""The codec + transport isolation matrix, run sequentially, one subprocess at a time.

Two experiments over one dataset (SPEC.md). Experiment 1 measures the two
columnar formats -- fpcolumn and Arrow IPC -- in one process: encode, emit the
bytes, decode, aggregate. Experiment 2 takes each emitted payload, byte for
byte, and moves it producer -> consumer over each transport -- POSIX shared
memory and AF_UNIX SOCK_SEQPACKET -- with no codec in the loop.

Sequential is the point, not an implementation detail. SPEC.md rule 1: timing
under load measures the neighbours. If cells overlapped -- two builds, two
payloads, two transports in flight -- every number would price contention
between the cells instead of the codec and the transport, which is exactly the
confusion this matrix exists to remove. So: one unit process at a time, start
to finish; the runner waits for each and collects its one JSON line.

The runner times nothing itself. Every number it prints is a field the units
reported; the only arithmetic here is formatting, plus the checks SPEC.md
requires before anything is reported: the five checksums must be identical in
every cell, and each transport must account for its whole payload
(chunks x chunk_bytes).

It builds what is missing (build_fpg.sh, build_transport.sh) when the binaries
are absent, then measures. A unit that is missing and cannot be built is a
named error, not a traceback: the three units are other agents' files.

Run it inside the dev image (SPEC.md rule 2), as the user that owns the tree
(the entrypoint's default `postgres` cannot write a bind-mounted tree owned by
your host user; the runner falls back to build/ if it has to):

    docker run --rm --shm-size=3g --entrypoint python3 --user "$(id -u):$(id -g)" \
        -v "$PWD":/src -w /src fastpostgres-dev \
        benchmark/codec_transport/run_matrix.py --quick

From outside the image it wraps every command in `docker run` with this tree
mounted at /src. The report goes to stdout and to docs/codec-transport.log
(relative to the repository root; --out overrides); if that file cannot be
created, the runner says so and writes build/codec-transport.log instead, so a
finished run is never lost.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# Where everything is. REPO comes from this file's own path, so the same
# runner works with the tree at /src (inside the image) and with the tree
# anywhere else (it then mounts the tree at /src for every command).
# ---------------------------------------------------------------------------

REPO = Path(__file__).resolve().parents[2]
CT_DIR = REPO / "benchmark" / "codec_transport"
BUILD_DIR = CT_DIR / "build"

DOCKER_IMAGE = os.environ.get("FASTPOSTGRES_IMAGE", "fastpostgres-dev")
DOCKER_SHM = os.environ.get("SHM", "3g")  # the justfile's SHM size, for the shm transport
TIMEOUT_S = 3600  # a full 1M-row cell is seconds; this is a hang detector, not a measurement
FULL_ROWS = 1_000_000
FULL_REPS = 3
QUICK_ROWS = 1000  # SPEC.md rule 1: smoke with tiny sizes, measure the matrix sequentially
QUICK_REPS = 1
DEFAULT_CHUNK = 1 << 20  # one MiB, the chunking the transport cells are measured at
DEFAULT_OUT = Path("docs") / "codec-transport.log"
CHECKSUM_KEYS = ("sum_id", "sum_stock", "sum_price_bits", "trues", "text_hash")


class Failure(Exception):
    """A condition worth a message, not a traceback."""


# ---------------------------------------------------------------------------
# The units (SPEC.md's CLI contract) and what to do when one is absent.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Unit:
    name: str
    path: Path
    build: Path | None  # None: the file is a script, nothing to compile
    owner: str  # who to tell when it is missing


UNITS = (
    Unit("fpg_bench", BUILD_DIR / "fpg_bench", CT_DIR / "build_fpg.sh",
         "the FPG codec agent (fpg_*)"),
    Unit("arrow_bench.py", CT_DIR / "arrow_bench.py", None,
         "the Arrow codec agent (arrow_*)"),
    Unit("transport_bench", BUILD_DIR / "transport_bench", CT_DIR / "build_transport.sh",
         "the transport agent (transport_*)"),
)


# ---------------------------------------------------------------------------
# Running commands: directly when we are already in the dev image, otherwise
# through `docker run` with the tree mounted at /src (SPEC.md rule 2).
# ---------------------------------------------------------------------------


def inside_dev_image() -> bool:
    """True when this process is the image's own command: the entrypoint is
    installed under /usr/local/bin and the image keeps the tree at /src."""
    return Path("/usr/local/bin/entrypoint.sh").exists() and Path("/src/pg-extension").is_dir()


class Machine:
    """Runs one unit process at a time, and refuses to run two at once."""

    def __init__(self) -> None:
        self.inside = inside_dev_image()
        self._busy = False
        if not self.inside and shutil.which("docker") is None:
            raise Failure(
                "this is not the dev image and `docker` is not on PATH.\n"
                "  Run it in the image instead:\n"
                "    docker run --rm -v \"$PWD\":/src -w /src fastpostgres-dev \\\n"
                "        python3 benchmark/codec_transport/run_matrix.py [--quick]"
            )

    def view(self, path: Path) -> str:
        """The path as the measured process will see it."""
        if self.inside:
            return str(path)
        return "/src/" + os.path.relpath(path, REPO)

    def run(self, argv: list[str], cwd: Path, what: str) -> str:
        """One subprocess, waited for. Returns its stdout."""
        if self._busy:
            raise RuntimeError(
                "two measurements in flight — the matrix is sequential by construction"
            )
        self._busy = True
        try:
            if self.inside:
                full, run_cwd = list(argv), str(cwd)
            else:
                full = ["docker", "run", "--rm", f"--shm-size={DOCKER_SHM}",
                        "-v", f"{REPO}:/src", "-w", self.view(cwd)]
                rev = os.environ.get("BENCH_GIT_REV")
                if rev:
                    full += ["-e", f"BENCH_GIT_REV={rev}"]
                full += [DOCKER_IMAGE, *argv]
                run_cwd = None
            try:
                done = subprocess.run(full, cwd=run_cwd, capture_output=True,
                                      text=True, errors="replace", timeout=TIMEOUT_S)
            except FileNotFoundError as exc:
                raise Failure(f"{what}: cannot execute {full[0]!r}: {exc}") from None
            except subprocess.TimeoutExpired:
                raise Failure(
                    f"{what}: no result after {TIMEOUT_S}s\n"
                    f"  command: {' '.join(full)}"
                ) from None
        finally:
            self._busy = False
        if done.returncode != 0:
            raise Failure(
                f"{what}: exited {done.returncode}\n"
                f"  command: {' '.join(full)}\n"
                f"  output:\n{indent(tail(done.stderr) or tail(done.stdout))}"
            )
        return done.stdout


def tail(text: str, lines: int = 12) -> str:
    kept = [line for line in text.splitlines() if line.strip()]
    return "\n".join(kept[-lines:])


def indent(text: str) -> str:
    return "\n".join("    " + line for line in text.splitlines()) or "    (no output)"


def parse_json(what: str, stdout: str) -> tuple[dict, str]:
    """The last JSON object line on stdout. The image's entrypoint prints an
    ENV block before the command it runs, so the JSON line is not always the
    only line; searching from the end skips that header either way."""
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(doc, dict):
            return doc, line
    raise Failure(f"{what}: no JSON object on stdout; output was:\n{indent(tail(stdout, 8))}")


def ensure_units(machine: Machine, dry: bool) -> list[str]:
    """Build the binaries that are missing; return the units still missing,
    each with a reason. Called before any measurement, so a build never runs
    while a cell is being timed."""
    missing: list[str] = []
    for unit in UNITS:
        if unit.path.exists():
            continue
        if unit.build is not None and unit.build.exists() and not dry:
            print(f"# building {unit.name} (bash {unit.build.name})", flush=True)
            machine.run(["bash", machine.view(unit.build)], cwd=CT_DIR,
                        what=f"build {unit.name}")
        if unit.path.exists():
            continue
        if unit.build is None:
            why = f"{unit.owner} owns it"
        elif not unit.build.exists():
            why = f"its build script {unit.build.name} is missing too"
        elif dry:
            why = f"would be built by {unit.build.name} (--dry-run did not run it)"
        else:
            why = f"{unit.build.name} ran but did not produce it"
        missing.append(f"{unit.name} ({machine.view(unit.path)}): {why}")
    return missing


# ---------------------------------------------------------------------------
# The cells, in the one order they run in: both codecs (which emit the
# payloads), then each payload over each transport.
# ---------------------------------------------------------------------------


@dataclass
class Task:
    name: str  # display name, e.g. "fpcolumn" or "fpcolumn + shm"
    kind: str  # "format" | "transport"
    label: str  # one line for the log
    argv: list[str]
    cwd: Path
    file: Path | None = None  # the payload this cell emits (format) or moves (transport)
    parent: str = ""  # the format cell whose payload a transport cell moves
    which: str = ""  # shm | uds (transport cells)


@dataclass
class Cell:
    name: str
    kind: str
    doc: dict  # the unit's parsed JSON
    raw: str  # the unit's JSON line, for the log
    file: Path | None = None
    parent: str = ""
    which: str = ""


def make_tasks(machine: Machine, rows: int, reps: int, chunk: int,
               payload_dir: Path) -> list[Task]:
    fpg = machine.view(BUILD_DIR / "fpg_bench")
    arrow = machine.view(CT_DIR / "arrow_bench.py")
    transport = machine.view(BUILD_DIR / "transport_bench")
    fpg_payload = payload_dir / "fpcolumn.bin"
    arrow_payload = payload_dir / "arrow-ipc.bin"
    size = ["--rows", str(rows), "--reps", str(reps)]

    tasks = [
        Task("fpcolumn", "format",
             f"fpcolumn: encode + decode in one process (rows={rows}, reps={reps})",
             [fpg, *size, "--format", "fpcolumn", "--emit-payload",
              machine.view(fpg_payload)], CT_DIR, file=fpg_payload),
        Task("arrow-ipc", "format",
             f"arrow-ipc: encode + decode in one process (rows={rows}, reps={reps})",
             ["python3", arrow, *size, "--emit-payload",
              machine.view(arrow_payload)], CT_DIR, file=arrow_payload),
    ]
    for parent, payload in (("fpcolumn", fpg_payload), ("arrow-ipc", arrow_payload)):
        for which in ("shm", "uds"):
            tasks.append(Task(
                f"{parent} + {which}", "transport",
                f"{parent} payload over {which} (chunk={chunk} B)",
                [transport, "--which", which, "--payload", machine.view(payload),
                 "--chunk", str(chunk)], CT_DIR, file=payload,
                parent=parent, which=which))
    return tasks


def make_payload_dir() -> Path:
    """A scratch directory for the emitted payloads, inside the build directory
    so that both the in-image and the docker-wrapped mode see the same file."""
    try:
        BUILD_DIR.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix="payloads-", dir=str(BUILD_DIR)))
    except OSError as exc:
        raise Failure(
            f"cannot create a payload directory under {BUILD_DIR}: {exc}\n"
            f"  (the build directory must be writable by the user running the "
            f"units; inside the image the entrypoint runs the command as postgres)"
        ) from None


def measure(machine: Machine, rows: int, reps: int, chunk: int, payload_dir: Path,
            report: "Report") -> list[Cell]:
    cells: list[Cell] = []
    for task in make_tasks(machine, rows, reps, chunk, payload_dir):
        report.line(f"# run: {task.label}")
        stdout = machine.run(task.argv, task.cwd, task.label)
        doc, raw = parse_json(task.label, stdout)
        cell = Cell(task.name, task.kind, doc, raw, file=task.file,
                    parent=task.parent, which=task.which)
        if task.kind == "format":
            check_emitted(cell)
        cells.append(cell)
    return cells


def check_emitted(cell: Cell) -> None:
    path = cell.file
    if path is None or not path.exists() or path.stat().st_size == 0:
        raise Failure(
            f"{cell.name}: --emit-payload produced no payload at {path} — "
            f"the transport cells would have nothing to move"
        )


# ---------------------------------------------------------------------------
# Validation: SPEC.md's gates, run before the report is printed.
# ---------------------------------------------------------------------------


def check(cells: list[Cell]) -> list[str]:
    """Returns notes; raises Failure on anything that makes the numbers
    incomparable or incomplete."""
    notes: list[str] = []

    # 1. The five checksums, identical in every cell (SPEC.md: "Every cell must
    #    report the same five numbers"). A format cell must report them; a
    #    transport cell that reports them must agree.
    baseline: dict | None = None
    baseline_name = ""
    for cell in cells:
        got = cell.doc.get("checksum")
        if not isinstance(got, dict) or any(key not in got for key in CHECKSUM_KEYS):
            if cell.kind == "format":
                raise Failure(f"{cell.name}: no complete checksum in its JSON: {got!r}")
            fold = cell.doc.get("payload_fold")
            if (cell.doc.get("verified") is True and fold is not None
                    and fold == cell.doc.get("payload_fold_reference")):
                notes.append(
                    f"{cell.name}: no logical checksum; it verified the payload bytes "
                    f"against its own fold reference instead"
                )
            else:
                notes.append(
                    f"{cell.name}: no checksum reported; the transport moved bytes but "
                    f"nothing here proves they decode to the same rows"
                )
            continue
        got = {key: got[key] for key in CHECKSUM_KEYS}
        if baseline is None:
            baseline, baseline_name = got, cell.name
        elif got != baseline:
            diff = ", ".join(
                f"{key}: {baseline[key]} != {got[key]}"
                for key in CHECKSUM_KEYS if baseline[key] != got[key]
            )
            raise Failure(
                f"checksum mismatch — '{cell.name}' carried different data than "
                f"'{baseline_name}': {diff}"
            )

    # 2. Every transport moved the whole payload: chunks x chunk_bytes must be
    #    the bytes the cell reports, and those must cover the file.
    for cell in cells:
        if cell.kind == "transport":
            notes.extend(check_moved(cell))
    return notes


def check_moved(cell: Cell) -> list[str]:
    if cell.file is None or not cell.file.exists():
        raise Failure(f"{cell.name}: the payload file is gone ({cell.file})")
    size = cell.file.stat().st_size
    doc = cell.doc
    chunks, chunk_bytes, reported = (
        doc.get("chunks"), doc.get("chunk_bytes"), doc.get("payload_bytes"))
    if chunks is None or chunk_bytes is None:
        raise Failure(
            f"{cell.name}: no chunk accounting in its JSON (chunks/chunk_bytes), so the "
            f"whole payload cannot be proven moved; got chunks={chunks!r}, "
            f"chunk_bytes={chunk_bytes!r}, payload_bytes={reported!r}"
        )
    if reported is None:
        raise Failure(f"{cell.name}: no payload_bytes in its JSON")
    chunks, chunk_bytes, reported = int(chunks), int(chunk_bytes), int(reported)
    if chunk_bytes <= 0:
        raise Failure(f"{cell.name}: chunk_bytes={chunk_bytes} is not a size")
    moved = chunks * chunk_bytes

    # The SPEC's accounting: chunks x chunk_bytes is what the cell moved and it
    # is what the cell reports. It has to cover the file.
    if moved == reported:
        if moved < size:
            raise Failure(
                f"{cell.name}: moved {moved} B for a {size} B payload — truncated "
                f"({cell.file})"
            )
        if moved > size:
            return [
                f"{cell.name}: {moved} B accounted for a {size} B payload; the final "
                f"chunk is wider than the file's tail, so the whole file moved"
            ]
        return []

    # The other common convention: chunk_bytes is the nominal chunk size, the
    # final chunk is short, and payload_bytes is what actually moved (= the
    # file). chunks x chunk_bytes is then an upper bound; what proves the move
    # complete is that the chunk count covers the file and the reported bytes
    # are the file's.
    covers = -(-size // chunk_bytes)
    if reported == size and moved >= size and chunks == covers:
        return [
            f"{cell.name}: {chunks} chunks x {chunk_bytes} B is an upper bound; "
            f"payload_bytes={reported} is the file and {chunks} chunks cover it "
            f"(the final chunk is short)"
        ]
    if moved < size or reported < size:
        raise Failure(
            f"{cell.name}: accounted {moved} B (payload_bytes={reported}) for a "
            f"{size} B payload — truncated ({cell.file})"
        )
    raise Failure(
        f"{cell.name}: the chunk accounting does not match the payload: "
        f"chunks={chunks}, chunk_bytes={chunk_bytes}, payload_bytes={reported}, "
        f"file={size} B"
    )


# ---------------------------------------------------------------------------
# The report: stdout and docs/codec-transport.log, line for line the same.
# ---------------------------------------------------------------------------


class Report:
    """stdout and the commit-able log are the same text, line for line."""

    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.fallback: tuple[Path, OSError] | None = None
        self._fh = open(path, "w", encoding="utf-8")

    def line(self, text: str = "") -> None:
        print(text, flush=True)
        print(text, file=self._fh, flush=True)

    def close(self) -> None:
        self._fh.close()


def open_report(preferred: Path) -> Report:
    """The report is opened before any measurement, so the log exists from the
    first line and a failure to open it cannot waste a run. The common failure
    is a bind-mounted tree the container user does not own (the image's
    entrypoint runs commands as postgres); then the report goes to build/,
    which is git-ignored, and the reason is printed loudly."""
    try:
        return Report(preferred)
    except OSError as exc:
        fallback = BUILD_DIR / preferred.name
        try:
            report = Report(fallback)
        except OSError:
            raise Failure(
                f"cannot write the report to {preferred} ({exc}) nor to {fallback}"
            ) from None
        report.fallback = (preferred, exc)
        print(
            f"run_matrix: cannot write the report to {preferred}: {exc}\n"
            f"run_matrix: writing it to {fallback} instead (git-ignored);\n"
            f"run_matrix: move it to {preferred} when that directory is writable "
            f"(or run with --out PATH / a writable user)",
            file=sys.stderr, flush=True,
        )
        return report


def resolve_out(args: argparse.Namespace) -> Path:
    if args.out:
        path = Path(args.out)
        return path if path.is_absolute() else Path.cwd() / path
    return REPO / DEFAULT_OUT


def git_rev() -> str:
    stamp = os.environ.get("BENCH_GIT_REV")
    if stamp:
        return stamp
    try:
        done = subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty"],
                              capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    if done.returncode != 0:
        return "unknown"
    return done.stdout.strip() or "unknown"


def cpu_model() -> str:
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def mem_total() -> str:
    try:
        with open("/proc/meminfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    return f"{float(line.split()[1]) / 1048576:.0f} GiB"
    except OSError:
        pass
    return "unknown"


def print_header(report: Report, machine: Machine, rows: int, reps: int, chunk: int,
                 quick: bool, payload_dir: Path) -> None:
    report.line("### CODEC + TRANSPORT ISOLATION MATRIX")
    report.line("# One subprocess at a time, start to finish: no two cells and no two payloads")
    report.line("# overlap, because a timed region that shares the machine with a neighbour")
    report.line("# measures the neighbour (SPEC.md rule 1). Nothing here talks to PostgreSQL.")
    report.line(f"mode: {'--quick smoke (not a measurement)' if quick else 'full matrix'}")
    report.line(f"rows: {rows}")
    report.line(f"cpu: {cpu_model()}")
    report.line(f"mem: {mem_total()}")
    report.line(f"reps: {reps} (format cells; the unit reports the median of the reps)")
    report.line(f"chunk: {chunk} B ({human_bytes(chunk)}) per transport message")
    report.line(f"units run: {'in the dev image' if machine.inside else f'via docker run ({DOCKER_IMAGE})'}")
    report.line(f"report: {report.path}")
    if report.fallback is not None:
        preferred, exc = report.fallback
        report.line(f"# note: {preferred} was not writable ({exc}); this is a fallback copy")
    report.line(f"payload dir: {payload_dir} (removed on success)")
    report.line(f"git: {git_rev()}")
    report.line("# matrix: formats {fpcolumn, arrow-ipc} x transports {shm, uds}; format cells are")
    report.line("# in-process (transport held out), transport cells move the emitted payloads")
    report.line("# (codec held out). Every number is a unit's own JSON field; the runner times")
    report.line("# nothing itself.")


def print_checks(report: Report, cells: list[Cell], notes: list[str]) -> None:
    with_checksum = sum(
        1 for cell in cells
        if isinstance(cell.doc.get("checksum"), dict)
        and all(key in cell.doc["checksum"] for key in CHECKSUM_KEYS)
    )
    report.line(
        f"# checks: {len(cells)} cells; checksums identical among the {with_checksum} "
        f"that reported them; every transport accounted for its whole payload"
    )
    first = next((cell for cell in cells if isinstance(cell.doc.get("checksum"), dict)
                  and all(key in cell.doc["checksum"] for key in CHECKSUM_KEYS)), None)
    if first is not None:
        checksum = first.doc["checksum"]
        report.line("# checksum: " + " ".join(f"{key}={checksum[key]}" for key in CHECKSUM_KEYS))
    for note in notes:
        report.line(f"# note: {note}")


def num(value, digits: int = 2) -> str:
    if value is None:
        return "—"
    try:
        return f"{float(value):.{digits}f}"
    except (TypeError, ValueError):
        return "—"


def ms_num(value) -> str:
    """Milliseconds with enough digits to stay readable: per-chunk latencies at
    small chunk sizes are microseconds, and 2 decimals would print 0.00."""
    if value is None:
        return "—"
    try:
        ms = float(value)
    except (TypeError, ValueError):
        return "—"
    if ms >= 1.0:
        return f"{ms:.2f}"
    if ms >= 0.01:
        return f"{ms:.3f}"
    return f"{ms:.4f}"


def rows_per_s(value) -> str:
    if value is None:
        return "—"
    rate = float(value)
    if rate >= 1e6:
        return f"{rate / 1e6:.2f}M"
    if rate >= 1e3:
        return f"{rate / 1e3:.1f}k"
    return f"{rate:.0f}"


def human_bytes(value) -> str:
    if value is None:
        return "—"
    size = float(value)
    if size >= 1 << 20:
        return f"{size / (1 << 20):.1f} MiB"
    if size >= 1 << 10:
        return f"{size / (1 << 10):.1f} KiB"
    return f"{size:.0f} B"


def integer(value) -> str:
    if value is None:
        return "—"
    try:
        return str(int(value))
    except (TypeError, ValueError):
        return "—"


def cell_size(cell: Cell) -> int | None:
    """The payload's bytes on disk: what the transports actually moved."""
    if cell.file is not None and cell.file.exists():
        return cell.file.stat().st_size
    return cell.doc.get("payload_bytes")


def print_tables(report: Report, cells: list[Cell], chunk: int) -> None:
    formats = [cell for cell in cells if cell.kind == "format"]
    transports = [cell for cell in cells if cell.kind == "transport"]

    report.line()
    report.line("### FORMAT — encode -> bytes -> decode + aggregates, one process, no transport (Experiment 1)")
    header = (f"{'cell':<12} {'payload':>10} {'rows/s':>9} {'GB/s':>7} "
              f"{'encode ms':>10} {'decode ms':>10} {'copies':>7}")
    report.line(header)
    report.line("-" * len(header))
    for cell in formats:
        report.line(
            f"{cell.name:<12} {human_bytes(cell_size(cell)):>10} "
            f"{rows_per_s(cell.doc.get('rows_per_s')):>9} "
            f"{num(cell.doc.get('gb_per_s')):>7} "
            f"{num(cell.doc.get('encode_ms'), 1):>10} "
            f"{num(cell.doc.get('decode_ms'), 1):>10} "
            f"{integer(cell.doc.get('copies')):>7}"
        )

    report.line()
    report.line("### TRANSPORT — the same payload bytes, producer -> consumer, codec out of the loop (Experiment 2)")
    report.line(f"# chunk = {chunk} B ({human_bytes(chunk)}); CPU is user+system over both sides")
    header = (f"{'cell':<18} {'payload':>10} {'chunks':>7} {'GB/s':>7} {'p50 ms':>8} "
              f"{'p99 ms':>8} {'CPU ms':>8} {'prod ms':>8} {'cons ms':>8} "
              f"{'RSS MiB':>8} {'copies':>6}")
    report.line(header)
    report.line("-" * len(header))
    for cell in transports:
        doc = cell.doc
        cpu = None
        if doc.get("cpu_user_ms") is not None or doc.get("cpu_sys_ms") is not None:
            cpu = float(doc.get("cpu_user_ms") or 0.0) + float(doc.get("cpu_sys_ms") or 0.0)
        rss_kb = doc.get("rss_kb")
        rss = f"{float(rss_kb) / 1024:.1f}" if isinstance(rss_kb, (int, float)) else "—"
        report.line(
            f"{cell.name:<18} {human_bytes(cell_size(cell)):>10} "
            f"{integer(doc.get('chunks')):>7} {num(doc.get('gb_per_s')):>7} "
            f"{ms_num(doc.get('p50_ms')):>8} {ms_num(doc.get('p99_ms')):>8} "
            f"{num(cpu, 1):>8} {num(doc.get('producer_cpu_ms'), 1):>8} "
            f"{num(doc.get('consumer_cpu_ms'), 1):>8} {rss:>8} "
            f"{integer(doc.get('copies')):>6}"
        )


def print_copies(report: Report, cells: list[Cell]) -> None:
    report.line()
    report.line("### COPIES — counted copies by stage, as the cells named them")
    report.line("# A 0 at a stage means the bytes were aliased or read in place there; the")
    report.line("# counts are the cells' own (SPEC.md: zero-copy is demonstrated by not copying).")
    for cell in cells:
        stages = cell.doc.get("copy_stages")
        if isinstance(stages, dict) and stages:
            text = ", ".join(f"{key or '(unnamed)'}={value}" for key, value in stages.items())
        else:
            text = "(no copy_stages reported)"
        report.line(f"  {cell.name:<18} copies={integer(cell.doc.get('copies'))}  {text}")


def print_raw(report: Report, cells: list[Cell]) -> None:
    report.line()
    report.line("### RAW — the units' JSON lines, verbatim, one per cell")
    for cell in cells:
        report.line(cell.raw)


# ---------------------------------------------------------------------------
# Entry point.
# ---------------------------------------------------------------------------


def positive(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not an integer") from None
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="run_matrix.py",
        description="Run the codec x transport isolation matrix sequentially and report it.",
        epilog="Every number comes from the units' own JSON; the runner times nothing itself.",
    )
    parser.add_argument("--rows", type=positive, default=FULL_ROWS,
                        help=f"rows for every cell (default {FULL_ROWS:,})")
    parser.add_argument("--chunk", type=positive, default=DEFAULT_CHUNK,
                        help=f"transport chunk size in bytes (default {DEFAULT_CHUNK})")
    parser.add_argument("--quick", action="store_true",
                        help=f"smoke only: rows={QUICK_ROWS}, reps={QUICK_REPS} "
                             f"(overrides --rows)")
    parser.add_argument("--out", metavar="PATH", default=None,
                        help="write the report here instead of docs/codec-transport.log "
                             "(a relative PATH is taken from the current directory)")
    parser.add_argument("--dry-run", action="store_true",
                        help="check the units and print the commands that would run; "
                             "execute nothing")
    return parser.parse_args(argv)


def plan(machine: Machine, rows: int, reps: int, chunk: int, missing: list[str]) -> int:
    """--dry-run: no subprocesses, no measurements, just the plan."""
    print("--dry-run: nothing was executed and nothing was timed")
    print(f"# rows={rows} reps={reps} chunk={chunk}")
    for unit in UNITS:
        if unit.path.exists():
            state = "present"
        elif unit.build is not None and unit.build.exists():
            state = f"missing; would run {unit.build.name}"
        else:
            state = "MISSING"
        print(f"# unit {unit.name:<16} {state:<28} {machine.view(unit.path)}")
    for task in make_tasks(machine, rows, reps, chunk, BUILD_DIR / "payloads-<tmp>"):
        print(f"# would run ({task.kind}): {' '.join(task.argv)}")
    if missing:
        print("--dry-run: missing units:\n  " + "\n  ".join(missing))
        return 1
    print("--dry-run: all units present")
    return 0


def run(args: argparse.Namespace) -> int:
    rows = QUICK_ROWS if args.quick else args.rows
    reps = QUICK_REPS if args.quick else FULL_REPS

    machine = Machine()
    missing = ensure_units(machine, dry=args.dry_run)
    if args.dry_run:
        return plan(machine, rows, reps, args.chunk, missing)
    if missing:
        raise Failure(
            "cannot measure the matrix — missing unit(s):\n"
            + "\n".join(f"  - {entry}" for entry in missing)
        )

    payload_dir = make_payload_dir()
    try:
        report = open_report(resolve_out(args))
    except Failure:
        shutil.rmtree(payload_dir, ignore_errors=True)
        raise

    failed = False
    try:
        print_header(report, machine, rows, reps, args.chunk, args.quick, payload_dir)
        cells = measure(machine, rows, reps, args.chunk, payload_dir, report)
        notes = check(cells)
        print_checks(report, cells, notes)
        print_tables(report, cells, args.chunk)
        print_copies(report, cells)
        print_raw(report, cells)
    except BaseException:
        failed = True
        raise
    finally:
        report.close()
        if payload_dir.exists():
            if failed:
                print(f"run_matrix: payloads kept for inspection: {payload_dir}",
                      file=sys.stderr)
            else:
                shutil.rmtree(payload_dir, ignore_errors=True)
    return 0


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        return run(args)
    except Failure as exc:
        print(f"run_matrix: {exc}", file=sys.stderr, flush=True)
        return 2
    except KeyboardInterrupt:
        print("run_matrix: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
