# fastpostgres (Python client)

The client for [FastPostgres](https://github.com/carlosdaniel26/fastpostgres):
PostgreSQL query results delivered through POSIX shared memory, read in place.

```python
import fastpostgres.dialect  # registers postgresql+fastpostgres
engine = create_engine("postgresql+fastpostgres://user@/app?host=/tmp")
```

The native module needs libpq and the PostgreSQL headers (`pg_config` on `PATH`),
and the extension tree beside this package: the wire format has one copy, in
`extension/`. Build from a checkout of the repository:

```
pip install ./python
```

The rest - what it is, the measured results, the limits - is
in the [repository](https://github.com/carlosdaniel26/fastpostgres).
