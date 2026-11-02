# FastPostgres dev tasks: one image, a throwaway PostgreSQL for every run.
#
#   just test                 the Python client's test suite
#   just bench [nrows ...]    fast-postgres vs ADBC + Arrow read benchmarks
#   just pgcache-bench [args] fast-postgres vs a result cache (PgCache), same rows
#   just pgcache-memory-bench [args] the same, with one memory budget on both sides
#   just demo [nrows]         the psycopg 3 vs fastpostgres driver swap
#   just charts [nrows ...]   run the benchmarks and render the charts
#   just decode-cycles [args] the row decode's cycles/row, on a fixed shape
#   just decode-ir [args]     the decode's dynamic instructions + miss profile
#   just cache-stat [rows] [reps]  cache counters on the host's PMU
#   just decode-mca [args]    the decode loop's ports/latency, offline (llvm-mca)
#   just codec-transport      format cost vs transport cost, isolated (2x2 matrix)
#   just codec-charts         charts for that matrix log
#
# This file is the frontend -- the only thing you run by hand. Everything else is
# either baked into the image or a host-side helper:
#
#   image/       the image: its Dockerfile (a dev stage and the profile stage
#                the offline meters need), its entrypoint, the jobs it runs
#   benchmark/   the benchmark harnesses          (run inside the image)
#   demo/        the app the demo switches drivers on   (run inside the image)
#   tools/       host-side helpers: tools/charts.py, which `just charts` calls
#
# Row counts default to each benchmark's own. Override the ring's shared-memory
# size with SHM=<size> (default 3g). The decode meters take their own arguments
# (`just decode-ir --shape mixed`); MCA_CPU picks the core decode-mca models.

set shell := ["bash", "-euo", "pipefail", "-c"]

image := env("IMAGE", "fastpostgres-dev")
shm := env("SHM", "3g")
# The log carries the commit it was produced from, so published numbers point
# back at the code that produced them; a "-dirty" suffix means the working tree
# had uncommitted changes when the run happened.
gitrev := `git describe --always --dirty 2>/dev/null || echo unknown`

# list the recipes
default:
    @just --list

# build the dev image (client tests, benchmarks and demo all run in it)
[private]
_image:
    docker build -q --target dev -t {{image}} -f image/Dockerfile . >/dev/null

# build the profile image: the dev image plus valgrind and llvm (the offline meters)
[private]
_image_profile:
    docker build -q --target profile -t {{image}}-profile -f image/Dockerfile . >/dev/null

# run a command in the image, on a fresh PostgreSQL with the extension
[private]
_run *args:
    just _image
    docker run --rm --shm-size={{shm}} -e FASTPOSTGRES_OBJ_ARENA={{env("OBJ_ARENA", "")}} -e SHAPE={{env("SHAPE", "")}} -e BENCH_GIT_REV={{gitrev}} {{image}} {{args}}

# the same, in the profile image (callgrind and llvm-mca live there)
[private]
_run_profile *args:
    just _image_profile
    docker run --rm --shm-size={{shm}} -e FASTPOSTGRES_OBJ_ARENA={{env("OBJ_ARENA", "")}} -e MCA_CPU={{env("MCA_CPU", "")}} -e SHAPE={{env("SHAPE", "")}} -e BENCH_GIT_REV={{gitrev}} {{image}}-profile {{args}}

# the Python client's test suite
test:
    just _run python3 -m unittest discover -s tests -v

# the read benchmarks: fast-postgres against ADBC + Arrow over the same rows
bench *sizes:
    just _run benchmarks.sh {{sizes}}

# the same SQLAlchemy app on psycopg 3 and on fastpostgres
demo n="5000":
    just _run demo.sh {{n}}

# benchmarks + charts, from one command
charts *sizes:
    tools/charts.py {{sizes}}

# the row decode's TSC cycles/row on a fixed shape (rows batches, --shape mixed)
decode-cycles *args:
    just _run decode-cycles.sh {{args}}

# the decode's dynamic instructions and miss profile (callgrind), and the IPC
decode-ir *args:
    just _run_profile decode-ir.sh {{args}}

# the decode loop's ports and latency, modeled offline by llvm-mca (MCA_CPU)
decode-mca *args:
    just _run_profile decode-mca.sh {{args}}

# cache counters on the host's PMU: decodes per row, transports per KiB (rows reps)
cache-stat *args:
    image/cache-stat.sh {{args}}

# before/after charts for a change, from the improve-*.log pair
ab-charts *args:
    tools/ab_charts.py {{args}}

# the codec x transport isolation matrix: format cost and transport cost,
# separated. Sequential by construction (see docs/CODEC-TRANSPORT.md); writes
# docs/codec-transport.log. Runs as the invoking user so the log is writable.
codec-transport *args:
    just _image
    docker run --rm --shm-size={{shm}} --entrypoint python3 --user "$(id -u):$(id -g)" -e BENCH_GIT_REV={{gitrev}} -v {{justfile_directory()}}:/src -w /src {{image}} benchmark/codec_transport/run_matrix.py {{args}}

# charts for the matrix log: format cost and transport cost, side by side
codec-charts *args:
    tools/codec_charts.py {{args}}

# fastpostgres against a result cache (PgCache) in front of the same server: the
# two regimes (large result, small expensive result) and the cache's two taxes
# (cold fill, pass-through). Redirect it to docs/fastpostgres-vs-pgcache.log.
pgcache-bench *args:
    tools/pgcache_bench.sh {{args}}

# the chart for that comparison: the full scan against the aggregate, one image
pgcache-charts *args:
    tools/pgcache_charts.py {{args}}

# fastpostgres against a result cache with the *same memory budget on both
# sides*: the ring's two transfer segments against PgCache's own memory limit
# and result memo, swept across the budget a result stops fitting in. Redirect
# it to docs/fastpostgres-vs-pgcache-memory.log
pgcache-memory-bench *args:
    tools/pgcache_memory_bench.sh {{args}}

# the chart for that sweep: the budget on the x axis, one image
pgcache-memory-charts *args:
    tools/pgcache_memory_charts.py {{args}}

# drop the built images
clean:
    -docker rmi {{image}} {{image}}-profile
