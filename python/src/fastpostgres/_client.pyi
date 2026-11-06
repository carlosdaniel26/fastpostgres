"""Type surface of the native client module (`fastpostgres._client`).

The implementation is C++; these stubs are what an editor and a type checker see.
They cover what the package itself uses - the DBAPI's connection and ring, and the
decoded segment it iterates - so `py.typed` does not promise more than it has.
"""
from typing import Any

class Connection:
    def __init__(self, conninfo: str) -> None: ...
    def exec(self, sql: str) -> Result: ...
    def close(self) -> None: ...

class Result:
    fields: list[str]
    field_types: list[int]
    rows: list[tuple[Any, ...]]
    row_count: int

class Ring:
    def __init__(self, segment: str, segment_bytes: int = ..., transport: str = ...) -> None: ...
    def start(self, conn: Connection) -> None: ...
    def open(self, seg: str, query: str) -> None: ...
    def next(self, batch: int = ...) -> int: ...
    def next_rows(self, batch: int = ...) -> int: ...
    def post_rows(self, seg: str, batch: int = ...) -> None: ...
    def wait(self) -> int: ...
    def close(self) -> None: ...
    def stop(self) -> None: ...

class Column:
    name: str
    kind: int
    pgtype: int
    nrows: int
    data: memoryview
    bytes: memoryview
    nulls: memoryview | None
    offsets: memoryview
    has_nulls: bool
    def value_at(self, index: int) -> Any: ...

class Segment:
    row_count: int
    columns: list[Column]

def read(seg: str) -> Segment: ...
def read_rows(seg: str) -> Result: ...

KIND_BOOL: int
KIND_INT: int
KIND_FLOAT: int
KIND_TEXT: int
KIND_DATE: int
KIND_TIME: int
KIND_TIMESTAMP: int
KIND_TIMESTAMPTZ: int
KIND_UUID: int
KIND_BYTEA: int
