CREATE FUNCTION fastpostgres_ring_start(text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_start'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION fastpostgres_ring_start(text, text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_start'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION fastpostgres_ring_stop(text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_stop'
LANGUAGE C STRICT VOLATILE;

-- Bulk-load a read-only SELECT into a sealed memfd in the FPCOLUMN layout.
-- Returns "FPQCOL1 <backend pid> <fd> <bytes> <rows>".  capacity 0 means the
-- 1 GiB segment default.  This two-argument form is the original /proc path.
CREATE FUNCTION fastpostgres_columnar_load(text, bigint DEFAULT 0) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_columnar_load'
LANGUAGE C STRICT VOLATILE;

-- The same load, handing the sealed descriptor to the client's AF_UNIX control
-- socket over SCM_RIGHTS.  The client publishes an abstract address ("@name");
-- the descriptor string above is unchanged, so either path can attach.
CREATE FUNCTION fastpostgres_columnar_load(text, bigint, text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_columnar_load'
LANGUAGE C STRICT VOLATILE;
