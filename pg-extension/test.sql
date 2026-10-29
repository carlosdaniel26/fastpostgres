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

\echo '--- direct heap scan (no SQL executor, no SPI) ---'
SELECT native_scan_shm('scan_seg', 'dst') AS scanned;
SELECT native_shm_info('scan_seg') AS info;

CREATE TABLE IF NOT EXISTS dst_copy (LIKE dst);
SELECT native_write_shm('scan_seg', 'dst_copy') AS scan_rows_written;
SELECT count(*) = 5000                         AS scan_copy_count_matches,
       (SELECT i8 FROM dst_copy WHERE i4 = 42) = 42 AS scan_sample_int8,
       (SELECT t  FROM dst_copy WHERE i4 = 42) = 'x42' AS scan_sample_text
FROM dst_copy;

\echo '--- scan: column selection + WHERE predicate ---'
SELECT native_scan_shm('scan_sel', 'dst', ARRAY['t', 'i8'], 'i8 >= 100') AS scanned_sel;

CREATE TABLE IF NOT EXISTS dst_sel (t text, i8 int8);
SELECT native_write_shm('scan_sel', 'dst_sel') AS sel_rows_written;
SELECT count(*) = 4901                          AS sel_count_matches,
       (SELECT min(i8) FROM dst_sel) = 100      AS sel_min_i8,
       (SELECT t FROM dst_sel WHERE i8 = 100) = 'x100' AS sel_sample
FROM dst_sel;
