CREATE FUNCTION fastpostgres_ring_start(text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_start'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION fastpostgres_ring_start(text, text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_start'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION fastpostgres_ring_stop(text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_stop'
LANGUAGE C STRICT VOLATILE;
