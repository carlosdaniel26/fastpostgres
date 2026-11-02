"""Build the client's native module.

The wire format it speaks lives in the extension's tree (`../extension`), which is
also where the server side includes it from: one copy of the header, two
consumers. Building from a checkout of the repository is the supported path;
the error below says so when the extension is not beside the client.
"""
import os
import subprocess

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))
PG_EXTENSION = os.path.normpath(os.path.join(HERE, os.pardir, "extension"))
if not os.path.isdir(PG_EXTENSION):
    raise SystemExit(
        "fastpostgres: the extension tree is missing.\n"
        f"  looked for {PG_EXTENSION}\n"
        "  the client builds from a checkout of the repository, where the\n"
        "  extension's headers are the single copy of the wire format."
    )


def pg_config(option):
    """Ask libpq's pg_config for a path, so the build works wherever it lands
    (/usr/include on Arch, /usr/include/postgresql on Debian)."""
    try:
        return subprocess.check_output(["pg_config", option], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


if pg_config("--includedir") is None:
    raise SystemExit(
        "fastpostgres: pg_config is not on PATH.\n"
        "  install PostgreSQL's client library and development headers\n"
        "  (postgresql-libs + postgresql on Arch, libpq-dev on Debian)."
    )

include_dirs = [PG_EXTENSION, os.path.join(PG_EXTENSION, "src")]
library_dirs = []
for option, target in (("--includedir", include_dirs), ("--libdir", library_dirs)):
    path = pg_config(option)
    if path:
        target.append(path)

setup(
    ext_modules=[
        Extension(
            "fastpostgres._client",
            sources=[
                "src/fastpostgres/native/addon.cpp",
                "src/fastpostgres/native/connection.cpp",
                "src/fastpostgres/native/ring.cpp",
                "src/fastpostgres/native/segment.cpp",
                "src/fastpostgres/native/codec.cpp",
                "src/fastpostgres/native/types.cpp",
            ],
            include_dirs=include_dirs,
            library_dirs=library_dirs,
            libraries=["pq"],
            language="c++",
            # -g: line tables for the offline meters (callgrind annotation and
            # the llvm-mca loop extractor both need them). Debug info only; the
            # generated code is the -O2 code, so what `just bench` measures is
            # what `just decode-mca` models.
            extra_compile_args=["-std=c++17", "-O2", "-g"],
        )
    ],
)
