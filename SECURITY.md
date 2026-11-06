# Security

## The trust boundary is `/dev/shm/<segment>`

The ring is an **SQL-execution endpoint**. A background worker maps a segment the
client created and executes whatever SQL arrives through it; there is no
authentication on the segment itself, because the segment is the boundary. The
client creates it `0600`, and `fastpostgres_shm_map_rw()` widens it to `0666`
(`extension/src/shm.cpp`) so a client running as a different OS user can map it.

So, by design:

- any local process that can open the segment can execute SQL through it, as the
  worker's database user. The segment name is not a secret, but the segment is:
  keep it out of shared or world-readable directories, and treat read access to
  `/dev/shm` as read access to the database;
- the worker exits when its client process is gone, and `fastpostgres_ring_stop()`
  stops it; nothing else reclaims a segment for you;
- writes, DDL and transactions do **not** travel through the ring - they stay on
  libpq, with PostgreSQL's own authentication.

This is a research prototype, not production software. Do not expose it to
untrusted users or networks. The full list of limits is in
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md).

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use GitHub's private
vulnerability reporting on this repository (Security -> Report a vulnerability),
or email the maintainer at the address in `python/pyproject.toml`.

Include what you can: the version of the extension and the wheel, what the
attacker can already do (a local user? SQL access? a segment they can open?), and
the smallest reproduction. Reports about the segment's trust model as documented
above are expected behaviour, not vulnerabilities - but a way to cross the
boundary *without* the segment, or to make the worker execute more than the
client asked for, is a real finding.
