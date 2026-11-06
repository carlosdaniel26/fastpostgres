"""The native materializer: flat declarative models without ORM hydration.

No database here — the contract is about the row frame, so the rows are made in
Python and the assertions are about the instances that come back and the
attributes the ORM's descriptors read out of them.
"""
import unittest

from sqlalchemy import Boolean, Float, Integer, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

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

    def test_row_length_mismatch_raises(self):
        with self.assertRaises(ValueError):
            materialize(Flat, [(1, "short")])

    def test_non_class_model_raises(self):
        with self.assertRaises(TypeError):
            materialize(Flat(), ROWS)

    def test_key_order_is_mapper_order_by_default(self):
        objs = materialize(Flat, ROWS)
        self.assertEqual(tuple(vars(objs[0])), tuple(attr.key for attr in Flat.__mapper__.column_attrs))


if __name__ == "__main__":
    unittest.main()
