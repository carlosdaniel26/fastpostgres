-- 1.1.0: the ring can be started on a named transport. The one-argument form
-- keeps starting the shm ring; this adds the two-argument overload so a client
-- can name the engine the worker is to run.
CREATE FUNCTION fastpostgres_ring_start(text, text) RETURNS text
AS 'MODULE_PATHNAME', 'fastpostgres_ring_start'
LANGUAGE C STRICT VOLATILE;
