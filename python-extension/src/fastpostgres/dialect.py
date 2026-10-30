"""SQLAlchemy dialect for fastpostgres.

Registering this makes ``create_engine("postgresql+fastpostgres://…")`` resolve
to the fastpostgres DB-API module, so an existing SQLAlchemy app changes only
its URL. Importing this module registers the dialect (installing the package
also does, via the ``sqlalchemy.dialects`` entry point).
"""
from typing import Any, Dict, List, Tuple

from sqlalchemy.dialects.postgresql.base import PGDialect


class PGDialect_fastpostgres(PGDialect):
    driver = "fastpostgres"
    supports_statement_cache = True

    @classmethod
    def import_dbapi(cls):
        from fastpostgres import dbapi

        return dbapi

    # SQLAlchemy 1.4 compatibility
    @classmethod
    def dbapi(cls):  # pragma: no cover
        return cls.import_dbapi()

    def create_connect_args(self, url) -> Tuple[List[Any], Dict[str, Any]]:
        query = dict(url.query)
        if "conninfo" in query:
            return [query["conninfo"]], {}

        opts = url.translate_connect_args(username="user")
        parts = []
        for key, name in (("host", "host"), ("port", "port"), ("database", "dbname"),
                          ("user", "user"), ("password", "password")):
            if opts.get(key) is not None:
                parts.append(f"{name}={opts[key]}")
        parts.extend(f"{k}={v}" for k, v in query.items())
        return [" ".join(parts)], {}


from sqlalchemy.dialects import registry  # noqa: E402

registry.register("postgresql.fastpostgres", "fastpostgres.dialect", "PGDialect_fastpostgres")
