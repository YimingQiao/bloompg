\echo Use "CREATE EXTENSION bloompg" to load this file. \quit

CREATE FUNCTION bloompg_version()
RETURNS text
AS 'MODULE_PATHNAME', 'bloompg_version'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bloompg_last_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'bloompg_last_trace'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION bloompg_last_profile()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'bloompg_last_profile'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

-- CREATE FUNCTION does not dlopen a C module.  Load it now so the installing
-- backend immediately receives the planner hook; other backends should use
-- shared_preload_libraries/session_preload_libraries or LOAD as documented.
SELECT bloompg_version();
