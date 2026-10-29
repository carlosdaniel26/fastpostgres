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

CREATE FUNCTION native_stream_open(text, text) RETURNS text
AS 'MODULE_PATHNAME', 'native_stream_open'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION native_stream_next(text, bigint DEFAULT NULL) RETURNS bigint
AS 'MODULE_PATHNAME', 'native_stream_next'
LANGUAGE C VOLATILE;

CREATE FUNCTION native_stream_close(text) RETURNS text
AS 'MODULE_PATHNAME', 'native_stream_close'
LANGUAGE C STRICT VOLATILE;
