"""fastpostgres.orm — flat declarative models, without SQLAlchemy's per-row hydration.

SQLAlchemy's ORM spends ~4 us per row building an entity: it allocates an
``InstanceState``, a weakref, runs the instrumentation, and writes each column
through an instrumented descriptor, with the identity map on the side.  When the
read only needs the columns of a flat model — no relationships, no lazy loads,
no change tracking — that is bookkeeping above the data, and the driver can
build the instances directly from its row tuples:

    from fastpostgres.orm import materialize

    rows = conn.fetchall(select(User))       # the driver's tuples
    users = materialize(User, rows)          # the same rows as User instances

**Two modes, one call.** ``materialize`` is only allowed to take the fast path
for a *flat* model.  If the mapper needs anything the fast path cannot honor,
``materialize`` returns ordinary, ORM-instrumented instances (real
``InstanceState``, real descriptors, no session) instead of a half-ORM object
whose missing state fails later.  The returned list has the same shape either
way; the instances tell you which mode ran:

    sqlalchemy.inspect(obj)          # fast: NoInspectionAvailable
                                     # fallback: a real InstanceState

The fast path is used only when **all** of these hold:

- no relationships (``mapper.relationships`` is empty) — so no lazy loading,
  eager loaders, backrefs or cascades;
- no mapper inheritance and no polymorphic mapping — the row cannot choose a
  subclass, and discriminator restoration is ORM state;
- no ``version_id_col`` — optimistic concurrency is change tracking;
- no attribute validators (``@validates``) — assignment should keep validating
  after the read;
- no composite properties;
- no deferred or raiseload columns — those are lazy loading of columns;
- every mapper property is a plain ``ColumnProperty`` over one column (no
  synonyms, no multi-column properties).

A partial projection is also routed to the fallback: if ``keys`` does not name
exactly the model's mapped column attributes (order may differ, set may not),
the fast path has no way to represent an unloaded attribute, so the caller gets
a stock instance whose unloaded attributes behave as SQLAlchemy defines.

What a **fast** materialized instance is:

- an instance of the model class, with the column attributes readable and
  ``vars(obj)`` holding exactly the row's columns;
- detached from any session: no identity map, no ``_sa_instance_state``;
- a plain attribute bag: ``obj.attr = value`` writes ``__dict__`` and records
  nothing, is never validated, and is never persisted.  Read path only.

What the **fallback** returns:

- a real ORM instance with an ``InstanceState``, built without calling
  ``__init__`` (like query-loading), populated directly into the instance dict
  (like query-loading, so validators do not run on the row values); for a
  polymorphic mapper, the row's discriminator selects the mapped subclass when
  the discriminator is among the materialized keys;
- *detached* when the primary key columns were materialized, otherwise
  transient.  A detached instance has an identity key but no session, so
  relationship access raises ``DetachedInstanceError`` — the stock behavior for
  a loaded-but-detached object, not a silent empty relationship.  Use
  ``session.merge(obj)`` to attach it;
- later attribute writes go through the normal instrumentation: validators
  fire, history is recorded, and the object is usable in a session.

**Defaults.** Column ``default=`` and ``server_default=`` are INSERT-time
values, not load-time values; stock ORM hydration does not apply them either.
Every materialized attribute comes from the row.  If a caller omits a defaulted
column via ``keys``, the fast path is not used (partial projection), and the
attribute remains unloaded/absent — it is never filled with the default.

**``__repr__`` and identity.** Neither mode adds a ``__repr__``.  The class's
own ``__repr__`` runs if it defines one; otherwise Python's default
``<module.Class object at 0x...>`` does.  Fast instances have no ORM identity:
two ``materialize`` calls produce distinct objects even for the same primary
key, because there is no identity map to unify them.  Fallback instances carry
an identity key but are not in an identity map either.

Use this on the read path, where the rows are a result set being written out or
serialized, not an object graph being edited.  Keys are the model's attribute
keys in row order; by default that is the model's mapped columns in mapper
order, which is what ``select(Model)`` returns.

``rows`` may be any sequence of row sequences.  The DBAPI's lazy ``ResultSet``
is decoded once in C (its ``to_tuples()``) before the fill, instead of being
walked row by row through its Python iterator.
"""
from . import _client

__all__ = ["materialize"]

# model -> (fast: bool, default_keys: tuple[str, ...], reason: str | None).
# The mapper's shape does not change after configuration, so one probe per class
# is enough; this keeps the fast path's per-call cost to a dict lookup.
_plans = {}


def _plan(model):
    """Return ``(fast, keys, reason)`` for a mapped class, cached."""
    try:
        return _plans[model]
    except KeyError:
        pass

    from sqlalchemy.orm import configure_mappers
    from sqlalchemy.orm.properties import ColumnProperty

    # A mapped class only has its instrumented descriptors installed once
    # SQLAlchemy has configured the mappers; without that, attribute reads see
    # ``impl is None``. A real application reaches this before its first query,
    # but materialize() may be the first ORM call in a lean process.
    configure_mappers()
    try:
        mapper = model.__mapper__
    except Exception as exc:  # UnmappedClassError / AttributeError
        raise TypeError("model must be a mapped class") from exc

    keys = tuple(attr.key for attr in mapper.column_attrs)
    reason = _fallback_reason(mapper, ColumnProperty)
    plan = (reason is None, keys, reason)
    _plans[model] = plan
    return plan


def _fallback_reason(mapper, ColumnProperty):
    """The first mapper feature the fast path cannot honor, or ``None``."""
    if mapper.relationships:
        return "relationships"
    if mapper.inherits is not None or mapper.polymorphic_on is not None:
        return "mapper inheritance/polymorphism"
    if mapper.version_id_col is not None:
        return "version_id_col"
    if getattr(mapper, "validators", None):
        return "attribute validators"
    if getattr(mapper, "composites", None):
        return "composite properties"
    for prop in mapper.attrs:
        if not isinstance(prop, ColumnProperty):
            return "non-column mapper property"
        if prop.deferred or getattr(prop, "raiseload", False):
            return "deferred/raiseload column"
        if len(prop.columns) != 1:
            return "multi-column property"
    return None


def _decoded_rows(rows):
    """Decode the driver's lazy ResultSet once, in C, before filling.

    ``ResultSet`` is a sequence, so the native materializer would accept it as
    is; ``PySequence_Fast`` then walks its Python row iterator and builds a
    throwaway list of tuples one ``decode_row`` at a time. Asking it for its
    tuples runs the whole frame decode in C and hands the materializer the list
    it wanted anyway, without changing the result.
    """
    try:
        from .dbapi import ResultSet
    except Exception:  # the ORM path does not require the DBAPI module
        return rows
    if isinstance(rows, ResultSet):
        return rows.to_tuples()
    return rows


def _stock_materialize(model, keys, rows):
    """Build real ORM instances for models the fast path cannot honor."""
    from sqlalchemy.orm import make_transient_to_detached
    from sqlalchemy.orm.attributes import instance_dict
    from sqlalchemy.orm.instrumentation import manager_of_class

    mapper = model.__mapper__
    managers = {model: manager_of_class(model)}
    pk_keys_by_class = {}
    # A polymorphic row names its class in the discriminator; stock ORM loading
    # restores that subclass, so the fallback must too. The base class is the
    # only choice when the discriminator is not among the materialized keys.
    polymorphic_key = None
    if mapper.polymorphic_on is not None:
        try:
            polymorphic_key = mapper.get_property_by_column(mapper.polymorphic_on).key
        except Exception:
            polymorphic_key = getattr(mapper.polymorphic_on, "key", None)

    def row_class(values):
        if polymorphic_key is None or polymorphic_key not in values:
            return model
        identity = values[polymorphic_key]
        for candidate in mapper.self_and_descendants:
            if candidate.polymorphic_identity == identity:
                return candidate.class_
        return model

    out = []
    for row in rows:
        values = tuple(row)
        if len(values) != len(keys):
            raise ValueError("row length does not match keys")
        cls = model
        if polymorphic_key is not None:
            by_key = dict(zip(keys, values))
            cls = row_class(by_key)
        manager = managers.get(cls)
        if manager is None:
            manager = managers[cls] = manager_of_class(cls)
        obj = manager.new_instance()
        state = instance_dict(obj)
        for key, value in zip(keys, values):
            state[key] = value
        pk_keys = pk_keys_by_class.get(cls)
        if pk_keys is None:
            pk_keys = pk_keys_by_class[cls] = tuple(
                column.key for column in cls.__mapper__.primary_key
            )
        # Row-loaded objects represent existing rows, so give them an identity
        # key when the primary key is present: detached, not transient. That is
        # the stock behavior a session would leave behind after expunge(), and
        # it makes relationship access raise instead of returning an empty
        # collection for a row that was never loaded.
        if pk_keys and all(key in state and state[key] is not None for key in pk_keys):
            try:
                make_transient_to_detached(obj)
            except Exception:
                # A model quirk should not turn a read into an error; the
                # instance still has real instrumentation, just transient state.
                pass
        out.append(obj)
    return out


def materialize(model, rows, keys=None):
    """Return ``rows`` as instances of ``model``.

    Fast for flat models; stock SQLAlchemy instances for models with
    relationships, inheritance, validators, deferred columns, composites or
    versioning (see the module docstring for the exact boundary). ``keys`` names
    the attribute each row position fills; it defaults to the model's mapped
    column keys in mapper order. The caller owns the row order (``select(Model)``
    gives mapper order), exactly as with any positional row.

    The fast path requires ``keys`` to cover every mapped column exactly once
    (any order); a partial projection falls back to stock instances.
    """
    if not isinstance(model, type):
        raise TypeError("model must be a mapped class")
    fast, default_keys, _reason = _plan(model)
    if keys is None:
        keys = default_keys
    else:
        if isinstance(keys, str):
            raise TypeError("keys must be a sequence of attribute-name strings")
        keys = tuple(keys)
        if not all(isinstance(key, str) for key in keys):
            raise TypeError("keys must be attribute-name strings")

    if fast and len(keys) == len(default_keys) and set(keys) == set(default_keys):
        return _client.materialize(model, keys, _decoded_rows(rows))
    return _stock_materialize(model, keys, _decoded_rows(rows))
