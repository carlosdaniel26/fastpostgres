"""The native materializer: flat declarative models without ORM hydration.

No database here — the contract is about the row frame, so the rows are made in
Python and the assertions are about the instances that come back and the
attributes the ORM's descriptors read out of them. The fallback boundary is
tested the same way: a mapper shape, not a server, decides the mode.
"""
import unittest

from sqlalchemy import Boolean, Float, ForeignKey, Integer, Text, inspect
from sqlalchemy.orm import (
    DeclarativeBase,
    Mapped,
    composite,
    mapped_column,
    relationship,
    synonym,
    validates,
)
from sqlalchemy.exc import NoInspectionAvailable
from sqlalchemy.orm.exc import DetachedInstanceError

from fastpostgres.orm import materialize


class Base(DeclarativeBase):
    pass


class Flat(Base):
    __tablename__ = "fp_orm_flat_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str | None] = mapped_column(Text)
    price: Mapped[float] = mapped_column(Float)
    stock: Mapped[int | None] = mapped_column(Integer)
    active: Mapped[bool] = mapped_column(Boolean)


class WithRepr(Base):
    __tablename__ = "fp_orm_repr_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)

    def __repr__(self):
        return f"WithRepr(id={self.id!r})"


class Defaulted(Base):
    __tablename__ = "fp_orm_default_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    n: Mapped[int] = mapped_column(Integer, default=7)


class Parent(Base):
    __tablename__ = "fp_orm_parent_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str | None] = mapped_column(Text)
    children = relationship("Child", back_populates="parent")


class Child(Base):
    __tablename__ = "fp_orm_child_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    parent_id: Mapped[int] = mapped_column(ForeignKey("fp_orm_parent_test.id"))
    parent = relationship("Parent", back_populates="children")


class Validated(Base):
    __tablename__ = "fp_orm_validated_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str | None] = mapped_column(Text)

    seen = []

    @validates("name")
    def _check_name(self, key, value):
        type(self).seen.append(value)
        if value == "bad":
            raise ValueError("bad name")
        return value


class Deferred(Base):
    __tablename__ = "fp_orm_deferred_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    note: Mapped[str | None] = mapped_column(Text, deferred=True)


class Versioned(Base):
    __tablename__ = "fp_orm_versioned_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    n: Mapped[int] = mapped_column(Integer)
    version: Mapped[int] = mapped_column(Integer, default=1)

    __mapper_args__ = {"version_id_col": version}


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __composite_values__(self):
        return self.x, self.y

    def __eq__(self, other):
        return isinstance(other, Point) and (other.x, other.y) == (self.x, self.y)

    def __repr__(self):
        return f"Point({self.x!r}, {self.y!r})"


class WithComposite(Base):
    __tablename__ = "fp_orm_composite_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    x: Mapped[int] = mapped_column(Integer)
    y: Mapped[int] = mapped_column(Integer)
    point = composite(Point, x, y)


class WithSynonym(Base):
    __tablename__ = "fp_orm_synonym_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    name: Mapped[str | None] = mapped_column(Text)
    alias = synonym("name")


class Poly(Base):
    __tablename__ = "fp_orm_poly_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    kind: Mapped[str] = mapped_column(Text)

    __mapper_args__ = {"polymorphic_on": kind, "polymorphic_identity": "base"}


class PolySub(Poly):
    __mapper_args__ = {"polymorphic_identity": "sub"}


class WithRaiseLoad(Base):
    __tablename__ = "fp_orm_raiseload_test"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    note: Mapped[str | None] = mapped_column(Text)


KEYS = ("id", "name", "price", "stock", "active")
ROWS = [
    (1, "first", 1.5, 10, True),
    (2, None, 2.5, None, False),
    (3, "third", 3.5, 30, True),
]


class MaterializeTest(unittest.TestCase):
    def test_instances_are_the_model_and_columns_read(self):
        objs = materialize(Flat, ROWS)

        self.assertEqual(len(objs), len(ROWS))
        for obj, row in zip(objs, ROWS):
            self.assertIs(type(obj), Flat)
            self.assertIsInstance(obj, Flat)
            self.assertEqual(vars(obj), dict(zip(KEYS, row)))
            # The ORM's instrumented descriptors read first from __dict__, which
            # is what makes the detached instance usable on the read path.
            for key, value in zip(KEYS, row):
                self.assertEqual(getattr(obj, key), value)
            # No InstanceState or anything else the ORM would need.
            self.assertNotIn("_sa_instance_state", vars(obj))

    def test_explicit_keys_follow_row_order(self):
        objs = materialize(
            Flat,
            [(1, "seven", 0.5, 7, True)],
            ("stock", "name", "price", "id", "active"),
        )
        obj = objs[0]
        self.assertEqual(obj.id, 7)
        self.assertEqual(obj.name, "seven")
        self.assertEqual(vars(obj)["stock"], 1)
        self.assertNotIn("_sa_instance_state", vars(obj))

    def test_row_length_mismatch_raises(self):
        with self.assertRaises(ValueError):
            materialize(Flat, [(1, "short")])

    def test_non_class_model_raises(self):
        with self.assertRaises(TypeError):
            materialize(Flat(), ROWS)

    def test_key_order_is_mapper_order_by_default(self):
        objs = materialize(Flat, ROWS)
        expected = tuple(attr.key for attr in Flat.__mapper__.column_attrs)
        self.assertEqual(tuple(vars(objs[0])), expected)

    def test_fast_instances_have_no_orm_identity(self):
        obj = materialize(Flat, ROWS[:1])[0]
        with self.assertRaises(NoInspectionAvailable):
            inspect(obj)
        other = materialize(Flat, ROWS[:1])[0]
        self.assertIsNot(obj, other)
        self.assertEqual(vars(obj), vars(other))

    def test_repr_is_the_models_own_or_pythons_default(self):
        obj = materialize(WithRepr, [(1,)])[0]
        self.assertEqual(repr(obj), "WithRepr(id=1)")

        flat = materialize(Flat, ROWS[:1])[0]
        self.assertRegex(repr(flat), r"^<.*Flat object at 0x[0-9a-f]+>$")

    def test_defaults_are_not_applied_on_read(self):
        obj = materialize(Defaulted, [(1, 99)])[0]
        self.assertEqual(obj.n, 99)  # the row, not the INSERT default of 7
        self.assertNotIn("_sa_instance_state", vars(obj))

    def test_empty_lazy_result_set_is_accepted(self):
        from fastpostgres.dbapi import ResultSet

        rows = ResultSet([], list(KEYS), [0] * len(KEYS), 0)
        self.assertEqual(materialize(Flat, rows), [])

    def test_partial_projection_uses_stock_instances(self):
        obj = materialize(Flat, [(1,)], ("id",))[0]
        self.assertIn("_sa_instance_state", vars(obj))
        self.assertTrue(inspect(obj).detached)
        self.assertEqual(obj.id, 1)
        # An unloaded column on a detached stock object asks for a session; it
        # must not silently look like a NULL column.
        with self.assertRaises(DetachedInstanceError):
            obj.name


class FallbackBoundaryTest(unittest.TestCase):
    def assert_stock(self, obj):
        self.assertIn("_sa_instance_state", vars(obj))
        self.assertIs(inspect(obj).mapper.class_, type(obj))

    def test_relationships_fall_back_and_are_detached(self):
        obj = materialize(Parent, [(1, "parent")])[0]
        self.assert_stock(obj)
        self.assertTrue(inspect(obj).detached)
        self.assertEqual(inspect(obj).key[1], (1,))
        self.assertEqual(obj.name, "parent")
        with self.assertRaises(DetachedInstanceError):
            obj.children

    def test_validators_fall_back_and_run_on_later_writes(self):
        Validated.seen.clear()
        obj = materialize(Validated, [(1, "from-row")])[0]
        self.assert_stock(obj)
        # Hydration populates the state dict directly, as stock query loading
        # does; validators are for assignment, not for replaying stored values.
        self.assertEqual(Validated.seen, [])
        with self.assertRaises(ValueError):
            obj.name = "bad"
        obj.name = "edited"
        self.assertEqual(Validated.seen, ["bad", "edited"])
        self.assertTrue(inspect(obj).attrs.name.history.added)

    def test_deferred_columns_fall_back(self):
        obj = materialize(Deferred, [(1, "note")])[0]
        self.assert_stock(obj)
        self.assertIn("note", vars(obj))

    def test_versioned_mappers_fall_back(self):
        obj = materialize(Versioned, [(1, 5, 2)])[0]
        self.assert_stock(obj)

    def test_composites_fall_back_and_read_correctly(self):
        obj = materialize(WithComposite, [(1, 2, 3)])[0]
        self.assert_stock(obj)
        self.assertEqual(obj.point, Point(2, 3))

    def test_synonyms_fall_back(self):
        obj = materialize(WithSynonym, [(1, "name")])[0]
        self.assert_stock(obj)
        self.assertEqual(obj.alias, "name")

    def test_polymorphic_mappers_fall_back(self):
        obj = materialize(Poly, [(1, "base")])[0]
        self.assert_stock(obj)
        self.assertIs(type(obj), Poly)

    def test_polymorphic_fallback_restores_the_subclass(self):
        obj = materialize(Poly, [(1, "sub")])[0]
        self.assert_stock(obj)
        self.assertIs(type(obj), PolySub)

    def test_raiseload_columns_fall_back(self):
        WithRaiseLoad.__mapper__.attrs.note.raiseload = True
        obj = materialize(WithRaiseLoad, [(1, "note")])[0]
        self.assert_stock(obj)

    def test_partial_projection_keeps_stock_row_semantics(self):
        obj = materialize(Parent, [(2,)], ("id",))[0]
        self.assert_stock(obj)
        self.assertTrue(inspect(obj).detached)


if __name__ == "__main__":
    unittest.main()
