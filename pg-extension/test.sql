-- Real-query test for the nativepostgress extension. Uses a single query that
-- fetches many PostgreSQL types at once.

CREATE EXTENSION nativepostgress;

\echo '--- many types in one query (summary) ---'
SELECT native_query_header($q$
  SELECT g::int4            AS i4,
         g::int8            AS i8,
         g::int2            AS i2,
         g::float4          AS f4,
         g::float8          AS f8,
         g::numeric(10,2)   AS num,
         (g % 2 = 0)        AS flag,
         ('user' || g)::text                     AS name,
         ('vc' || g)::varchar(50)                AS vc,
         ('c' || g)::char(8)                     AS ch,
         ('2020-01-01'::date + g)::date          AS d,
         ('2020-01-01'::timestamptz + (g || ' days')::interval) AS ts
  FROM generate_series(1, 1000) g
$q$) AS summary;

\echo '--- write the result straight into shared memory ---'
SELECT native_query_shm('np_test', $q$
  SELECT g::int4            AS i4,
         g::int8            AS i8,
         g::int2            AS i2,
         g::float4          AS f4,
         g::float8          AS f8,
         g::numeric(10,2)   AS num,
         (g % 2 = 0)        AS flag,
         ('user' || g)::text                     AS name,
         ('vc' || g)::varchar(50)                AS vc,
         ('c' || g)::char(8)                     AS ch,
         ('2020-01-01'::date + g)::date          AS d,
         ('2020-01-01'::timestamptz + (g || ' days')::interval) AS ts
  FROM generate_series(1, 1000) g
$q$) AS shm_write;

\echo '--- shm header info ---'
SELECT native_shm_info('np_test') AS info;

\echo '--- round-trip: shm payload == direct bytea? ---'
SELECT native_shm_read('np_test') = native_query($q$
  SELECT g::int4            AS i4,
         g::int8            AS i8,
         g::int2            AS i2,
         g::float4          AS f4,
         g::float8          AS f8,
         g::numeric(10,2)   AS num,
         (g % 2 = 0)        AS flag,
         ('user' || g)::text                     AS name,
         ('vc' || g)::varchar(50)                AS vc,
         ('c' || g)::char(8)                     AS ch,
         ('2020-01-01'::date + g)::date          AS d,
         ('2020-01-01'::timestamptz + (g || ' days')::interval) AS ts
  FROM generate_series(1, 1000) g
$q$) AS roundtrip_equal;

\echo '--- write path: columnar -> shm -> native_write_shm ---'
CREATE TABLE IF NOT EXISTS dst (i4 int4, i8 int8, f8 float8, b bool, t text);

SELECT native_query_shm('wseg', $q$
  SELECT g::int4, g::int8, g::float8, (g % 2 = 0), ('x' || g)::text
  FROM generate_series(1, 5000) g
$q$) AS produced;

SELECT native_write_shm('wseg', 'dst') AS rows_written;

SELECT count(*) = 5000                     AS dst_has_5000,
       (SELECT i8 FROM dst WHERE i4 = 42) = 42 AS sample_int8,
       (SELECT t  FROM dst WHERE i4 = 42) = 'x42' AS sample_text
FROM dst;

\echo '--- write path: TOASTed text survives the round-trip (detoast) ---'
CREATE TABLE IF NOT EXISTS dst_toast (id int4, t text);
INSERT INTO dst_toast
  SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 50) g;

SELECT native_query_shm('toast_seg', 'SELECT id, t FROM dst_toast')
  AS produced_toast;

CREATE TABLE IF NOT EXISTS dst_toast_copy (LIKE dst_toast);
SELECT native_write_shm('toast_seg', 'dst_toast_copy') AS toast_rows_written;
SELECT count(*) = 50 AND min(length(t)) = 9600 AS toast_roundtrip
FROM dst_toast_copy;

\echo '--- streaming: pull batches through a cursor (needs an open transaction) ---'
BEGIN;
CREATE TEMP TABLE stream_batches (n int8);
CREATE TABLE IF NOT EXISTS dst_stream (LIKE dst);

SELECT native_stream_open('stream_seg', 'SELECT * FROM dst') AS opened;

INSERT INTO stream_batches SELECT native_stream_next('stream_seg', 2000);
SELECT native_write_shm('stream_seg', 'dst_stream');
INSERT INTO stream_batches SELECT native_stream_next('stream_seg', 2000);
SELECT native_write_shm('stream_seg', 'dst_stream');
INSERT INTO stream_batches SELECT native_stream_next('stream_seg', 2000);
SELECT native_write_shm('stream_seg', 'dst_stream');
INSERT INTO stream_batches SELECT native_stream_next('stream_seg', 2000);

SELECT native_stream_close('stream_seg') AS closed;
COMMIT;

SELECT count(*) = 5000                               AS stream_total,
       (SELECT sum(n) FROM stream_batches) = 5000    AS stream_rows_ok,
       (SELECT count(*) FROM stream_batches) = 4     AS stream_batches_ok,
       (SELECT i8 FROM dst_stream WHERE i4 = 42) = 42 AS stream_sample_int8,
       (SELECT t  FROM dst_stream WHERE i4 = 42) = 'x42' AS stream_sample_text
FROM dst_stream;
