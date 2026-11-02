-- The extension's SQL surface, exercised by pg_regress (`make installcheck`).
--
-- The ring itself needs a client that maps the segment, so the round trip is the
-- Python suite's job (python/tests). What a regression test can prove is what the
-- server installs: the extension, its version, the functions it adds, and that
-- the control file and the SQL file agree on the version.

CREATE EXTENSION fastpostgres;

SELECT extname, extversion FROM pg_extension WHERE extname = 'fastpostgres';

SELECT proname, pg_get_function_identity_arguments(oid) AS args
FROM pg_proc
WHERE proname LIKE 'fastpostgres\_%'
ORDER BY proname, args;

-- All are STRICT VOLATILE C functions: the ring's start (one overload for the
-- shm default, one that names a transport) and its stop.
SELECT proname, provolatile, proisstrict, pronargs
FROM pg_proc
WHERE proname LIKE 'fastpostgres\_%'
ORDER BY proname, pronargs;

DROP EXTENSION fastpostgres;

SELECT count(*) FROM pg_proc WHERE proname LIKE 'fastpostgres\_%';
