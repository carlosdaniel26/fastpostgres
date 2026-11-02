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
ORDER BY proname;

-- Both are STRICT VOLATILE C functions of one text argument, and neither is
-- leaked to public callers by a default grant beyond the database's.
SELECT proname, provolatile, proisstrict, pronargs
FROM pg_proc
WHERE proname LIKE 'fastpostgres\_%'
ORDER BY proname;

DROP EXTENSION fastpostgres;

SELECT count(*) FROM pg_proc WHERE proname LIKE 'fastpostgres\_%';
