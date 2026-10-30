# One image for the client tests, both benchmarks and the demo: postgres:18 with
# the fastpostgres extension installed, the Python client built, and SQLAlchemy +
# psycopg2 for the driver comparison.
#
# Build from the repository root (each scripts/*-docker.sh does it):
#   docker build -t fastpostgres-dev -f scripts/Dockerfile .
FROM postgres:18
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 python3 python3-dev python3-setuptools \
      python3-sqlalchemy python3-psycopg2 \
 && rm -rf /var/lib/apt/lists/*

COPY pg-extension /src/pg-extension
COPY python-extension /src/python-extension
COPY benchmark /src/benchmark
COPY demo /src/demo
COPY scripts/with-db.sh /usr/local/bin/with-db.sh
RUN chmod +x /usr/local/bin/with-db.sh

WORKDIR /src/pg-extension
RUN make clean && make && make install

WORKDIR /src/python-extension
RUN python3 setup.py build_ext --inplace

ENV PYTHONPATH=/src/python-extension
ENV DATABASE_URL="host=/tmp dbname=postgres user=postgres"
ENTRYPOINT ["/usr/local/bin/with-db.sh"]
