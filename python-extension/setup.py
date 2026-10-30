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
                "src/fastpostgres/_client.cpp",
                "src/fastpostgres/synth.cpp",
            ],
            include_dirs=include_dirs,
            library_dirs=library_dirs,
            libraries=["pq"],
            language="c++",
            extra_compile_args=["-std=c++17", "-O2"],
        )
    ]
)
