# Contributing

Thanks for looking. This project has opinions about how a measurement is made,
and they are the same opinions as the CI's, so a change that follows them is a
change that lands.

## Build and test

Docker and [`just`](https://github.com/casey/just) are the only host
requirements; everything else runs in `docker/Dockerfile`.

```bash
just test          # pg_regress (the extension) and the client suite, in the image
just check-links   # every markdown link and cross-file anchor resolves
```

`just lint` runs the check above; CI adds `ruff` (the conservative rule set
in `python/pyproject.toml`) and `shellcheck` over the harnesses. The C and C++
sources are formatted with the repository's `.clang-format`, and the
`.pre-commit-config.yaml` wires all of it for `pre-commit install`.

## Changing the wire format

The format lives in **one** header, `extension/include/fastpostgres/`, and both
sides include it: the extension's worker writes it, the Python client and the
codec benchmark read it. The doorbell has its own seam,
`extension/transport/transport.h` — one ops table, recorded in the ring, so a
segment whose magic, version or engine does not match is refused rather than
driven. If you change the layout, bump
`FASTPOSTGRES_SHM_MAGIC`/`VERSION` in `extension/include/fastpostgres/ring.h` or
`fastpostgres_shm.h` as the comment there says, and say why in the commit: a
stale segment must be refused, not misread.

## Measurements

Every published number is a committed record with a `git:` stamp of the code
that produced it. The rules are in
[`bench/METHODOLOGY.md`](bench/METHODOLOGY.md), and in short:

- one player per process, one player at a time;
- the same output on both sides, checked by the harness (a faster path that
  returned something else fails the run);
- the evidence line proves the path (reads served by the ring against reads
  issued, the budget the cache resolved, the bytes each side held);
- the record is rebuilt by its harness (`bench/RESULTS.md` says which), never
  edited by hand - a re-run replaces it, and the stamp changes.

A new benchmark belongs to one of the three classes in
[`bench/README.md`](bench/README.md); if it does not fit, say so in the
issue and we will find the class together.

## Releases

There are none, and the tree carries no release machinery for them: no changelog,
no tag, no release workflow. Two tools insist on a version string anyway -
PostgreSQL wants `default_version` in `extension/fastpostgres.control` with the
SQL file named after it, and setuptools wants one in `python/pyproject.toml` - and
all of them say `0.0.0`, which is the point.

## Pull requests

- One concern per PR; a move and a behaviour change in one diff are impossible to
  review together.
- Say what you measured, with the log, if the change is about performance.
- CI must be green: lint, `tests.sh` (pg_regress + client), and the workload
  smoke run, which checks digests and publishes no numbers.
