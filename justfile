# FastPostgres dev tasks: one image, a throwaway PostgreSQL for every run.
#
# Measurement has three classes, one command each (bench/README.md is the
# catalog: what each answers, the method, and where its record lives):
#
#   just rivals [target]     the competitive comparisons: ADBC + Arrow, and a
#   just internals [target]  the meters behind the design notes: decode cycles /
#   just adoption [target]   a published third-party suite, on its own terms:
#                            MagicStack's pgbench, with our cell in its roster
#   just charts [target]     one figure per log
#   just presentation        the deck set, rendered in the presentation style
#
# Plus the client suite and the demo:
#
#   just test                 the Python client's test suite
#   just demo [nrows]         the psycopg 3 vs fastpostgres driver swap
#
# This file is the frontend -- the only thing you run by hand. Everything else is
# either baked into the image or a host-side helper:
#
#   docker/      the image: its Dockerfile (a dev stage and the profile stage
#                the offline meters need) and its entrypoint
#   bench/       the harnesses, the class dispatchers, the chart scripts, the
#                image-side jobs, and the committed records under results/
#   extension/   the PostgreSQL extension;  python/  the client package
#   examples/    the app the demo switches drivers on   (run inside the image)

#
# Row counts default to each harness's own.
# Override the ring's shared-memory size with SHM=<size> (default 3g); MCA_CPU
# picks the core decode-mca models.

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
    docker build -q --target dev -t {{image}} -f docker/Dockerfile . >/dev/null

# build the profile image: the dev image plus valgrind and llvm (the offline meters)
[private]
_image_profile:
    docker build -q --target profile -t {{image}}-profile -f docker/Dockerfile . >/dev/null

# run a command in the image, on a fresh PostgreSQL with the extension
[private]
_run *args:
    just _image
    docker run --rm --shm-size={{shm}} -e SHAPE={{env("SHAPE", "")}} -e PG_OPTS={{env("PG_OPTS", "")}} -e BENCH_GIT_REV={{gitrev}} {{image}} {{args}}


# build the free-threaded image: the client compiled for CPython 3.13t
[private]
_image_nogil:
    docker build -q --target nogil -t {{image}}-nogil -f docker/Dockerfile . >/dev/null



# both test suites: the extension's SQL surface (pg_regress) and the client's
test:
    just _run tests.sh

# the competitive benchmarks: adbc | arrow-memory
rivals target="" *args:
    bench/dispatch/rivals.sh {{target}} {{args}}


# a published third-party suite, on its own terms: magicstack-pgbench
adoption target="" *args:
    bench/dispatch/adoption.sh {{target}} {{args}}

# the same SQLAlchemy app on psycopg 3 and on fastpostgres
demo n="5000":
    just _run run.sh {{n}}

# one figure per log: arrow | arrow-memory
charts target="" *args:
    bench/dispatch/charts.sh {{target}} {{args}}

# the deck set: the same numbers, rendered in the chosen presentation style
presentation *args:
    bench/dispatch/presentation.sh {{args}}

# the ADBC + Arrow read comparison (rivals adbc): fast-postgres against the
# Arrow ecosystem's own answer over the same rows
[private]
bench-adbc *sizes:
    just _run benchmarks.sh {{sizes}}

# fastpostgres against ADBC + Arrow with the ring's buffer fixed (rivals
# arrow-memory): the same reads and what each side held while it delivered them
[private]
arrow-memory-bench *args:
    bench/harness/arrow_memory_bench.sh {{args}}








# the adopted suite in the dev image (adoption magicstack-pgbench): the suite's
# own protocol over its Python roster, with our cell added to that roster
[private]
adoption-bench *args:
    just _run adoption.sh {{args}}

# the release version agrees in pyproject.toml, the control file and sql/
# every markdown link and cross-file anchor resolves
check-links:
    @python3 tools/check_links.py

# what CI's lint job adds on top: ruff (python/), shellcheck (the harnesses)
lint: check-links

# drop the built images
clean:
    -docker rmi {{image}} {{image}}-profile
