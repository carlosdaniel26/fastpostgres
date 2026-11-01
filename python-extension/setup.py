import os
import subprocess

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))
PG_EXTENSION = os.path.join(HERE, os.pardir, "pg-extension")


def pg_config(option):
    """Ask libpq's pg_config for a path, so the build works wherever it lands
    (/usr/include on Arch, /usr/include/postgresql on Debian)."""
    try:
        return subprocess.check_output(["pg_config", option], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


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
                "fastpostgres/native/addon.cpp",
                "fastpostgres/native/connection.cpp",
                "fastpostgres/native/ring.cpp",
                "fastpostgres/native/segment.cpp",
                "fastpostgres/native/codec.cpp",
                "fastpostgres/native/types.cpp",
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
