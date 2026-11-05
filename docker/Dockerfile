# One image for the client tests, both benchmarks and the demo: postgres:18 with
# the fastpostgres extension installed, the Python client built, and SQLAlchemy +
# psycopg 3 for the harness's own DDL and the PgCache proxy client. Its second
# stage adds the offline meters (valgrind, llvm) and is what the decode-ir/
# decode-mca recipes build.
#
# Build from the repository root (the justfile's `_image`/`_image_profile`
# recipes do it, with the target named):
#   docker build --target dev -t fastpostgres-dev -f docker/Dockerfile .
FROM postgres:18 AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
      python3-venv python3-sqlalchemy python3-psycopg python3-psycopg-c python3-numpy python3-pip \
 && rm -rf /var/lib/apt/lists/*

# The Arrow rival the benchmark measures against: neither pyarrow nor the ADBC
# drivers are in Debian, so they come from PyPI. Benchmark-only - the client and
# the extension never import them, and the dev image would still build and test
# without this layer.
RUN python3 -m pip install --break-system-packages --no-cache-dir \
      pyarrow adbc_driver_postgresql

# The adopted suite's driver roster (bench/adoption/): its Python runner imports
# every driver it can select, so all of them have to be present for any of its
# cells to run. Benchmark-only, like the Arrow layer above; the versions a record
# was produced with are in that record's own block.
RUN python3 -m pip install --break-system-packages --no-cache-dir \
      aiopg asyncpg distro py-postgresql psycopg2-binary uvloop

COPY extension /src/extension
COPY python /src/python
COPY bench /src/bench
COPY examples /src/examples
COPY docker/entrypoint.sh docker/tests.sh examples/run.sh \
     bench/jobs/adoption.sh bench/jobs/decode-cycles.sh bench/jobs/decode-ir.sh \
     bench/jobs/decode-mca.sh bench/jobs/client-scaling.sh bench/jobs/client-nogil.sh \
     bench/jobs/loop-asm.py bench/rivals/adbc-arrow/benchmarks.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/*.sh /usr/local/bin/loop-asm.py

# The adopted suite's Python cells run through its own wrapper, which wants a
# virtualenv beside it (`make -C _python venv`, in its README). The venv is made
# here instead, with the system's packages visible, so the roster installed above
# is the one install; the suite's own requirements.txt stays in the vendored tree.
RUN python3 -m venv --system-site-packages \
      /src/bench/adoption/magicstack-pgbench/_python/venv

WORKDIR /src/extension
RUN make clean && make && make install

WORKDIR /src/python
RUN python3 setup.py build_ext --inplace

# The tests run as postgres (the entrypoint's user): pg_regress writes results/
# under the extension's test tree, and the client suite imports the package.
RUN mkdir -p /src/extension/test/results && chown -R postgres:postgres /src/extension /src/python

ENV PYTHONPATH=/src/python/src
ENV DATABASE_URL="host=/tmp dbname=postgres user=postgres"
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

# The offline meters: valgrind (callgrind, for `just decode-ir`) and llvm
# (llvm-mca, for `just decode-mca`). The tests, the benchmarks and the demo do
# not need either, so they are a stage of their own: `--target profile`, tagged
# fastpostgres-dev-profile. The dev layers are shared, so this costs one RUN.
FROM dev AS profile
RUN apt-get update && apt-get install -y --no-install-recommends \
      valgrind llvm \
 && rm -rf /var/lib/apt/lists/*

# The free-threaded interpreter (PEP 703), for the client's own parallelism: the
# decode holds the GIL in the default build, so N threads on N partitions only
# decode in parallel here. uv installs a python-build-standalone build (headers
# included) and the client compiles against it in place — the ABI tag differs
# (cpython-313t), so both builds sit side by side and each interpreter finds its
# own. `--target nogil`, tagged fastpostgres-dev-nogil.
FROM dev AS nogil
# The interpreter and the venv must be readable by the container's user (postgres
# runs the command), so neither can live under /root where uv puts them by default.
ENV UV_PYTHON_INSTALL_DIR=/opt/uv-python
RUN python3 -m pip install --break-system-packages --no-cache-dir uv \
 && uv python install 3.13t \
 && uv venv --python 3.13t --seed /opt/nogil \
 && /opt/nogil/bin/python -m pip install --no-cache-dir numpy sqlalchemy setuptools \
 && chmod -R a+rX /opt/uv-python /opt/nogil
WORKDIR /src/python
RUN /opt/nogil/bin/python setup.py build_ext --inplace \
 && chown -R postgres:postgres /src/python
