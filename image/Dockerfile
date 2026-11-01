# One image for the client tests, both benchmarks and the demo: postgres:18 with
# the fastpostgres extension installed, the Python client built, and SQLAlchemy +
# psycopg2 for the driver comparison. Its second stage adds the offline meters
# (valgrind, llvm) and is what the decode-ir/decode-mca recipes build.
#
# Build from the repository root (the justfile's `_image`/`_image_profile`
# recipes do it, with the target named):
#   docker build --target dev -t fastpostgres-dev -f image/Dockerfile .
FROM postgres:18 AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
      python3-sqlalchemy python3-psycopg2 python3-numpy python3-pip \
 && rm -rf /var/lib/apt/lists/*

# The Arrow rival the benchmark measures against: neither pyarrow nor the ADBC
# drivers are in Debian, so they come from PyPI. Benchmark-only - the client and
# the extension never import them, and the dev image would still build and test
# without this layer.
RUN python3 -m pip install --break-system-packages --no-cache-dir \
      pyarrow adbc_driver_postgresql

COPY pg-extension /src/pg-extension
COPY python-extension /src/python-extension
COPY benchmark /src/benchmark
COPY demo /src/demo
COPY image/entrypoint.sh image/benchmarks.sh image/demo.sh \
     image/decode-cycles.sh image/decode-ir.sh image/decode-mca.sh \
     image/loop-asm.py /usr/local/bin/
RUN chmod +x /usr/local/bin/*.sh /usr/local/bin/loop-asm.py

WORKDIR /src/pg-extension
RUN make clean && make && make install

WORKDIR /src/python-extension
RUN python3 setup.py build_ext --inplace

ENV PYTHONPATH=/src/python-extension
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
