# A native materializer for flat declarative models

**Verdict: it works, and the win is large — but it is a semantic win, not a
transport win.** Building flat SQLAlchemy model instances directly from the
driver's row tuples skips the ORM's per-attribute hydration, `InstanceState`,
weakref, and identity map: **3361.0 -> 421.3 ns/row end to end (-87.5%,
7.98x)**, with the instance fill itself at **168.9 ns/row** (19.9x the stock
ORM row). The price is every session/relationship/change-tracking guarantee an
ORM exists to provide. This is a read-path escape hatch for flat rows, not an
ORM replacement.

Measured on `bench/orm/materialize_bench.py` at 100k rows, same
`postgresql+fastpostgres` driver for both cells; the full A/B is in
`bench/results/team-unix-ab.log`.

| cell | ns/row | rows/s |
|---|---|---|
| driver tuples (`fetchall`, no model) | 215.4 | 4.64M |
| **stock SQLAlchemy ORM** (`Session.execute(select(Row)).scalars()`) | **3361.0** | **0.30M** |
| **native materializer, end to end** (read + fill) | **421.3** | **2.37M** |
| native materializer, fill only (rows already in memory) | 168.9 | 5.92M |

The same driver read is 215.4 ns/row, so SQLAlchemy's ORM share is ~3.1 us/row
and is what this removes; the materializer adds ~206 ns/row on top of the driver
read. (The absolute ORM number here is lower than the 5.4 us in `bench/NOTES.md`;
that record came from a different harness/version. Both cells in this A/B use
the same script and image pair, so the delta is the measurement.)

## What is bypassed

For each row, stock ORM hydration does roughly: allocate the entity through
SQLAlchemy's `_initialize_instance`, create an `InstanceState` (a weakref plus
bookkeeping), register it with the session's identity map, then assign six
columns through `InstrumentedAttribute.__set__`, each maintaining committed
state/history and firing instrumentation. The materializer does none of it:

1. `python/src/fastpostgres/native/materialize.cpp` allocates the instance with
   the class's own `tp_alloc` — no `__init__`, no SQLAlchemy `_initialize_instance`.
2. It gets the instance `__dict__` with `PyObject_GenericGetDict` and inserts
   the column values with `PyDict_SetItem`. This is exactly where the
   instrumented descriptor looks first (`impl.supports_population` and the key
   in `__dict__`), so `obj.sku`, `obj.price`, `vars(obj)` and `repr(obj)` all
   work without any state.
3. No `InstanceState`, no weakref, no session, no identity map, no history, no
   events, no flush.

`python/src/fastpostgres/orm.py` is the thin Python face:

```python
from fastpostgres.orm import materialize

rows = conn.fetchall(select(User))   # the driver's tuples
users = materialize(User, rows)      # same rows, User instances
```

The wrapper resolves the default keys from `model.__mapper__.column_attrs`
(mapper order, which is what `select(Model)` returns), calls
`configure_mappers()` once so the descriptors exist, and hands
`(model, keys, rows)` to the C++ function. A caller with a non-default
projection or a renamed column passes `keys` explicitly, in row order.

## What is given up

A materialized object is an instance of the model class with readable column
attributes, and nothing else. It is deliberately **detached and read-only**:

- **No session integration.** `session.add()` / `merge()` / `delete()` need an
  `InstanceState` and will fail; there is no identity map, so two rows with the
  same primary key are two distinct objects; no expiry, refresh, or `flush()`.
- **No relationships.** No lazy/eager loading, backrefs, `selectinload`, or
  cascades; relationship attributes are absent from `__dict__` and their
  descriptors would need the state that is not there.
- **No change tracking.** Nothing is dirty, `session.commit()` cannot see these
  objects, and assigning a mapped attribute after materialization goes through
  the instrumented `__set__` and raises (read path only).
- **No construction hooks.** `__init__`, dataclass `__post_init__`, column
  defaults, validators, ORM events, and hybrids are bypassed. The values are
  only what the query returned.
- **No ORM introspection.** `sqlalchemy.inspect(obj)` and anything reading
  `obj._sa_instance_state` fail; the object is not a SQLAlchemy-managed entity.
- **Flat, plain models only.** Models with a `__dict__` (the normal declarative
  case); a `__slots__` model is refused. Deferred columns, column properties,
  synonyms, composite attributes, and polymorphic loading are out of scope.
  Keys are attribute keys in row order; a projection that reorders or renames
  columns must pass matching `keys`.

## Why it is fast, and what it is not

The object and its dict still have to be born: ~169 ns/row at 100k rows, and
~149 ns/row on a small table where the allocator is warmer. That is the floor
for any path that returns real Python model instances, and it is why this is
not a zero-cost trick. What disappears is the ~3 us/row of SQLAlchemy glue above
that floor. The driver's own tuple decode (~215 ns/row) remains untouched
because the prototype starts from `fetchall()` rows.

A further step would be to materialize directly from `stream()`'s columnar
segment, skipping the tuple list: column offsets and the null bitmap are already
there, so the model fill could consume them in the same C++ pass. That would
attack part of the 215 ns driver read, not the 3 us ORM share, so it is a
second-order optimization; it was left out to keep this slice measurable and
small.

## Honest reading

- For a **read-only flat result** headed for JSON, a report, a CSV, a template,
  or a batch job — the case where the ORM is only being used as a row mapper —
  this is a real 8x end-to-end improvement, and the code is small.
- For an application that **edits objects** or relies on relationships,
  identity, lazy loading, cascades, or events, this path cannot be used at all.
  It does not make SQLAlchemy faster; it steps outside SQLAlchemy.
- The ORM is the largest single cost in the pipeline precisely because it is
  doing semantics the driver cannot see. Removing those semantics is the win,
  and it is only honest to call that what it is.

The prototype is `materialize.cpp` (~150 lines), `orm.py`, the A/B harness under
`bench/orm/`, and `python/tests/test_orm.py` (column access, NULLs, explicit key
order, mismatched rows, non-class models, and no `_sa_instance_state`).
