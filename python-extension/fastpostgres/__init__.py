"""fastpostgres — Python client for the shared-memory transport.

`read()` returns a `Segment` whose columns are zero-copy `memoryview`s aliasing
the mapped ``/dev/shm`` segment; they are only valid until that segment is
rewritten by the next streamed chunk.
"""
from enum import IntEnum

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
    """Wire kinds, matching `FastpostgresKind` in fastpostgres_shm.h."""

    BOOL = 1
    INT32 = 2
    INT64 = 3
    FLOAT8 = 4
    TEXT = 5
    INT16 = 6
    FLOAT4 = 7
