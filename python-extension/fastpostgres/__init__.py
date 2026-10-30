"""fastpostgres — Python client for the shared-memory transport.

`read()` returns a `Segment` whose columns are zero-copy `memoryview`s aliasing
the mapped ``/dev/shm`` segment; they are only valid until that segment is
rewritten by the next streamed chunk.
"""
from enum import IntEnum

from . import _client
from ._client import (
    Column,
    Connection,
    Result,
    Ring,
    Segment,
    read,
    synth_copy,
    synth_segment,
)

__all__ = [
    "Column",
    "Connection",
    "Kind",
    "Result",
    "Ring",
    "Segment",
    "read",
    "synth_copy",
    "synth_segment",
]


class Kind(IntEnum):
    """Wire kinds, taken from FastpostgresKind in fastpostgres_shm.h."""

    BOOL = _client.KIND_BOOL
    INT32 = _client.KIND_INT32
    INT64 = _client.KIND_INT64
    FLOAT8 = _client.KIND_FLOAT8
    TEXT = _client.KIND_TEXT
    INT16 = _client.KIND_INT16
    FLOAT4 = _client.KIND_FLOAT4
