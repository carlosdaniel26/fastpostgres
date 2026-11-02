# Contributing

Thanks for looking. This project has opinions about how a measurement is made,
and they are the same opinions as the CI's, so a change that follows them is a
change that lands.

## Build and test

Docker and [`just`](https://github.com/casey/just) are the only host
requirements; everything else runs in `docker/Dockerfile`.

```bash
just test          # pg_regress (the extension) and the client suite, in the image
just check-version # the release version agrees in its three places
just check-links   # every markdown link and cross-file anchor resolves
```

`just lint` runs the two checks above; CI adds `ruff` (the conservative rule set
in `python/pyproject.toml`) and `shellcheck` over the harnesses. The C and C++
sources are formatted with the repository's `.clang-format`, and the
`.pre-commit-config.yaml` wires all of it for `pre-commit install`.

## Changing the wire format

The format lives in **one** header, `extension/include/fastpostgres/`, and both
sides include it: the extension's worker writes it, the Python client and the
codec benchmark read it. The doorbell has its own seam,
`extension/transport/transport.h` — two engines, one ops table, selected per
connection (`FASTPOSTGRES_TRANSPORT`) and recorded in the ring, so a ring made by
one engine is refused by the other. If you change the layout, bump
`FASTPOSTGRES_SHM_MAGIC`/`VERSION` in `extension/include/fastpostgres/ring.h` or
`fastpostgres_shm.h` as the comment there says, and say why in the commit: a
stale segment must be refused, not misread.

## Measurements

Every published number is a committed record with a `git:` stamp of the code
that produced it. The rules are in
[`docs/METHODOLOGY.md`](docs/METHODOLOGY.md), and in short:

- one player per process, one player at a time;
- the same output on both sides, checked by the harness (a faster path that
  returned something else fails the run);
- the evidence line proves the path (reads served by the ring against reads
  issued, the budget the cache resolved, the bytes each side held);
- the record is rebuilt by its harness (`docs/RESULTS.md` says which), never
  edited by hand - a re-run replaces it, and the stamp changes.

A new benchmark belongs to one of the three classes in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md); if it does not fit, say so in the
issue and we will find the class together.

## Releases

The version lives in three places and `just check-version` proves they agree:
`python/pyproject.toml`, `extension/fastpostgres.control`, and the
`extension/sql/fastpostgres--<version>.sql` file name. To release: bump all
three, add the `CHANGELOG.md` entry, tag `v<version>` - the release workflow
builds the extension tarball and the wheel and attaches them with checksums.

## Pull requests

- One concern per PR; a move and a behaviour change in one diff are impossible to
  review together.
- Say what you measured, with the log, if the change is about performance.
- CI must be green: lint, `tests.sh` (pg_regress + client), and the workload
  smoke run, which checks digests and publishes no numbers.
