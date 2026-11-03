"""Helpers every benchmark harness shares.

The harnesses live in three classes - workloads, rivals, internals - and each one
used to carry its own copy of these. They are here once so a change to how a
percentile or an RSS reading is computed lands everywhere at the same time.
"""
import os


def rss_bytes():
    """Peak resident set of this process: VmHWM, in bytes.

    Peak, not current, and the same reading the transport cells report as
    `rss_kb`, so RSS means one thing across the repository.
    """
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) * 1024
    return 0


def percentile(samples, q):
    """The q-th percentile of a list of numbers, nearest-rank."""
    if not samples:
        return 0.0
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, int(q * (len(ordered) - 1)))]


def same_digest(left, right, tolerance=1e-6):
    """Two digests agree: integers exactly, floats within a relative tolerance.

    A sum of `numeric` values arrives as Decimal on one side and float on the
    other, and the orders differ, so the last bits cannot be compared directly.
    """
    for a, b in zip(left, right):
        if isinstance(a, float) or isinstance(b, float):
            scale = abs(a) + abs(b)
            if scale and abs(a - b) > tolerance * scale:
                return False
        elif a != b:
            return False
    return True


def mapped_segment_bytes(pid=None):
    """Bytes of the ring data segments a client process has mapped."""
    stem = f"fp_dbapi_{pid if pid is not None else os.getpid()}_"
    total = 0
    for name in os.listdir("/dev/shm"):
        if name.startswith(stem) and "_seg" in name:
            total += os.stat(os.path.join("/dev/shm", name)).st_size
    return total
