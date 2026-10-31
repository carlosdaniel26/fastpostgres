# FastPostgres dev tasks: one image, a throwaway PostgreSQL for every run.
#
#   just test                 the Python client's test suite
#   just bench [nrows ...]    transport + SQLAlchemy benchmarks
#   just demo [nrows]         the psycopg2 vs fastpostgres driver swap
#   just charts [nrows ...]   run the benchmarks and render the charts
#
# This file is the frontend -- the only thing you run by hand. Everything else is
# either baked into the image or a host-side helper:
#
#   image/       the dev image: its Dockerfile, its entrypoint, the jobs it runs
#   benchmark/   the benchmark harnesses          (run inside the image)
#   demo/        the app the demo switches drivers on   (run inside the image)
#   tools/       host-side helpers: tools/charts.py, which `just charts` calls
#
# Row counts default to each benchmark's own. Override the ring's shared-memory
# size with SHM=<size> (default 3g).

set shell := ["bash", "-euo", "pipefail", "-c"]

image := env("IMAGE", "fastpostgres-dev")
shm := env("SHM", "3g")

# list the recipes
default:
    @just --list

# build the image (client tests, benchmarks and demo all run in it)
[private]
_image:
    docker build -q -t {{image}} -f image/Dockerfile . >/dev/null

# run a command in the image, on a fresh PostgreSQL with the extension
[private]
_run *args:
    just _image
    docker run --rm --shm-size={{shm}} {{image}} {{args}}

# the Python client's test suite
test:
    just _run python3 -m unittest discover -s tests -v

# transport + SQLAlchemy benchmarks
bench *sizes:
    just _run benchmarks.sh {{sizes}}

# the same SQLAlchemy app on psycopg2 and on fastpostgres
demo n="5000":
    just _run demo.sh {{n}}

# benchmarks + charts, from one command
charts *sizes:
    tools/charts.py {{sizes}}

# build and run the SIMD micro-benchmarks (freestanding, no container)
kernels:
    @mkdir -p build/simd
    cc -O3 -Isimd -o build/simd/kernels_bench simd/kernels_bench.c simd/kernels.c simd/cpu.c simd/kernels_avx2.S simd/kernels_avx512.S
    cc -O3 -Isimd -o build/simd/layout_bench simd/layout_bench.c
    @./build/simd/kernels_bench
    @echo
    @./build/simd/layout_bench

# drop the built image
clean:
    -docker rmi {{image}}
