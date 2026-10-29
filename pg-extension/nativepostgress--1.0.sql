CREATE FUNCTION native_ring_start(text) RETURNS text
AS 'MODULE_PATHNAME', 'native_ring_start'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_ring_stop(text) RETURNS text
AS 'MODULE_PATHNAME', 'native_ring_stop'
LANGUAGE C STRICT VOLATILE;
