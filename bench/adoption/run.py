#!/usr/bin/env python3
"""The adopted suite's runner, with our cell in its roster.

MagicStack's `pgbench` keeps its roster in a module-level list and, per entry,
execs a runner named after the implementation's platform prefix. The suite is
vendored verbatim, so this file extends the roster instead of editing it:
`fastpostgres-dbapi` is appended - its runner is the symlink beside theirs,
`pgbench_fastpostgres` - and then the suite's own `main()` runs with whatever
arguments this file was given, including its `--benchmark` name filters.

    python3 bench/adoption/run.py --pghost=127.0.0.1 --pguser=postgres \
        --warmup-time=3 --duration=10 'python-asyncpg' 'fastpostgres-dbapi'

The suite's Go and Node cells are in the vendored tree too; they need toolchains
the dev image does not carry, so the classes here name only the Python roster.
"""
import importlib.util
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

SUITE = Path(__file__).resolve().parent / "magicstack-pgbench"
OURS = "fastpostgres-dbapi"


def load_suite():
    loader = SourceFileLoader("magicstack_pgbench", str(SUITE / "pgbench"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def main(argv):
    suite = load_suite()
    if OURS not in suite.BENCHMARKS:
        suite.BENCHMARKS = [*suite.BENCHMARKS, OURS]
    sys.argv = ["pgbench", *argv[1:]]
    suite.main()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
