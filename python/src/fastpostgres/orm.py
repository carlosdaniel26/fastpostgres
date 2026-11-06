"""fastpostgres.orm — flat declarative models, without SQLAlchemy's per-row hydration.

SQLAlchemy's ORM spends ~4 us per row building an entity: it allocates an
``InstanceState``, a weakref, runs the instrumentation, and writes each column
through an instrumented descriptor, with the identity map on the side. When the
read only needs the columns of a flat model — no relationships, no lazy loads,
no change tracking — that is bookkeeping above the data, and the driver can
build the instances directly from its row tuples:

    from fastpostgres.orm import materialize

    rows = conn.fetchall(select(User))       # the driver's tuples
    users = materialize(User, rows)          # the same rows as User instances

``materialize`` allocates each instance with the class's allocator (no
``__init__``), puts the column values in its ``__dict__`` where SQLAlchemy's
instrumented descriptors look first, and appends it. That is the whole cost:
one object and one dict per row, filled in C++.

What a materialized instance is:

- an instance of the model class, with the column attributes readable and
  ``vars(obj)`` holding exactly the row's columns;
- detached: no session, no identity map, no ``_sa_instance_state``.

What it is **not** (by design, and the reason it is fast):

- not addable to a session (``session.add`` / ``merge`` need a state), and not
  subject to the identity map, expiry, or dirty tracking;
- no relationships, lazy loading, backrefs, hybrid properties, column
  defaults, ``__init__``, validators, or ORM events;
- no ``sqlalchemy.inspect(obj)``: there is no ``InstanceState``.

Use it on the read path, where the rows are a result set being written out or
serialized, not an object graph being edited. Keys are the model's attribute
keys in row order; by default that is the model's mapped columns in mapper
order, which is what ``select(Model)`` returns.
"""
from . import _client

__all__ = ["materialize"]

_configured = set()


def materialize(model, rows, keys=None):
    """Return ``rows`` as instances of ``model`` — flat models only.

    ``keys`` names the attribute each row position fills; it defaults to the
    model's mapped column keys in mapper order. The caller owns the row order
    (``select(Model)`` gives mapper order), exactly as with any positional row.
    """
    if not isinstance(model, type):
        raise TypeError("model must be a mapped class")
    if keys is None:
        keys = [attr.key for attr in model.__mapper__.column_attrs]
    else:
        keys = tuple(keys)
        if not all(isinstance(key, str) for key in keys):
            raise TypeError("keys must be attribute-name strings")
    # A mapped class only has its instrumented descriptors installed once
    # SQLAlchemy has configured the mappers; without that, attribute reads see
    # ``impl is None``. A real application reaches this before its first query,
    # but materialize() may be the first ORM call in a lean process.
    if model not in _configured:
        from sqlalchemy.orm import configure_mappers

        configure_mappers()
        _configured.add(model)

    return _client.materialize(model, keys, rows)
