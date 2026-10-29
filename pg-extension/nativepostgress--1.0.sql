CREATE FUNCTION native_query(text) RETURNS bytea
AS 'MODULE_PATHNAME', 'native_query'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_query_header(text) RETURNS text
AS 'MODULE_PATHNAME', 'native_query_header'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_query_shm(text, text) RETURNS text
AS 'MODULE_PATHNAME', 'native_query_shm'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_shm_read(text) RETURNS bytea
AS 'MODULE_PATHNAME', 'native_shm_read'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_shm_info(text) RETURNS text
AS 'MODULE_PATHNAME', 'native_shm_info'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_write_shm(text, text) RETURNS bigint
AS 'MODULE_PATHNAME', 'native_write_shm'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_scan_shm(text, text, text[] DEFAULT NULL, text DEFAULT NULL)
RETURNS text
AS 'MODULE_PATHNAME', 'native_scan_shm'
LANGUAGE C VOLATILE;
