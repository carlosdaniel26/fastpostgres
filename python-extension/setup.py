import os
import platform
import subprocess

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext as _build_ext

HERE = os.path.dirname(os.path.abspath(__file__))
PG_EXTENSION = os.path.join(HERE, os.pardir, "pg-extension")
SIMD = os.path.join(HERE, os.pardir, "simd")

# The marshalling kernels are shared with the extension (../simd) and are
# freestanding, so they are compiled here exactly as the extension compiles
# them. The assembly is System V AMD64 only.
X86 = platform.machine() in ("x86_64", "amd64")
SIMD_C = [os.path.join(SIMD, "cpu.c"), os.path.join(SIMD, "kernels.c")]
SIMD_ASM = (
    [os.path.join(SIMD, "kernels_avx2.S"), os.path.join(SIMD, "kernels_avx512.S")] if X86 else []
)


def pg_config(option):
    """Ask libpq's pg_config for a path, so the build works wherever it lands
    (/usr/include on Arch, /usr/include/postgresql on Debian)."""
    try:
        return subprocess.check_output(["pg_config", option], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


class build_ext(_build_ext):
    """Compile the shared kernels into this build's own object dir.

    distutils' src_extensions has no '.S', so it would refuse the assembly
    outright; compiling both the C and the asm here also keeps these objects
    separate from the extension's copy of the same sources.
    """

    def build_extension(self, ext):
        os.makedirs(self.build_temp, exist_ok=True)
        objects = list(ext.extra_objects or [])
        cc = self.compiler.compiler_so

        for src, extra in [(s, ["-O3"]) for s in SIMD_C] + [(s, []) for s in SIMD_ASM]:
            obj = os.path.join(self.build_temp, os.path.basename(src).rsplit(".", 1)[0] + ".o")
            self.announce("building kernel %s" % src, level=3)
            subprocess.check_call(cc + ["-fPIC", "-I" + SIMD] + extra + ["-c", src, "-o", obj])
            objects.append(obj)

        ext.extra_objects = objects
        super().build_extension(ext)


include_dirs = [PG_EXTENSION, os.path.join(PG_EXTENSION, "src"), SIMD]
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
            extra_compile_args=["-std=c++17", "-O2"],
        )
    ],
    cmdclass={"build_ext": build_ext},
)
