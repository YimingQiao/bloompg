#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "${script_dir}/.." && pwd)

pg_host=${PGHOST:-/tmp}
pg_port=${PGPORT:-5432}
pg_database=${PGDATABASE:-bloompg_tpch10}
psql_bin=${PSQL_BIN:-psql}
createdb_bin=${CREATEDB_BIN:-createdb}
duckdb_bin=${DUCKDB_BIN:-duckdb}
source_db=${TPCH_DUCKDB:-}
create_indexes=${TPCH_CREATE_INDEXES:-1}

if ! command -v "${psql_bin}" >/dev/null 2>&1 ||
   ! command -v "${createdb_bin}" >/dev/null 2>&1; then
    echo "PostgreSQL client binaries are unavailable; set PSQL_BIN and CREATEDB_BIN" >&2
    exit 1
fi
if ! command -v "${duckdb_bin}" >/dev/null 2>&1; then
    echo "DuckDB is unavailable; set DUCKDB_BIN or add duckdb to PATH" >&2
    exit 1
fi
if [[ -z "${source_db}" ]]; then
    echo "Set TPCH_DUCKDB to a populated TPC-H SF10 DuckDB database" >&2
    exit 1
fi
if [[ ! -f "${source_db}" ]]; then
    echo "TPC-H SF10 DuckDB source is unavailable: ${source_db}" >&2
    exit 1
fi

database_exists=$(
    "${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d postgres -X -qAt \
        -v database_name="${pg_database}" -f - <<'SQL'
SELECT 1 FROM pg_database WHERE datname = :'database_name';
SQL
)
if [[ "${database_exists}" != "1" ]]; then
    "${createdb_bin}" -h "${pg_host}" -p "${pg_port}" "${pg_database}"
fi

table_exists=$(
    "${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X -qAt \
        -c "SELECT to_regclass('public.lineitem') IS NOT NULL"
)
if [[ "${table_exists}" == "t" ]]; then
    echo "Refusing to append: ${pg_database}.public.lineitem already exists" >&2
    exit 1
fi

"${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X \
    -v ON_ERROR_STOP=1 -f "${project_root}/scripts/tpch_pg_schema.sql"

tables=(region nation supplier customer part partsupp orders lineitem)
for table_name in "${tables[@]}"; do
    echo "Loading ${table_name}..." >&2
    "${duckdb_bin}" "${source_db}" -readonly \
        -c "COPY ${table_name} TO '/dev/stdout' (FORMAT CSV, HEADER false, DELIMITER '|');" \
        | PGOPTIONS='-c synchronous_commit=off' \
          "${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X -q \
              -v ON_ERROR_STOP=1 \
              -c "COPY ${table_name} FROM STDIN WITH (FORMAT CSV, DELIMITER '|');"
done

if [[ "${create_indexes}" == "1" ]]; then
    echo "Building PostgreSQL TPC-H key indexes..." >&2
    "${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X \
        -v ON_ERROR_STOP=1 -f "${project_root}/scripts/tpch_pg_indexes.sql"
elif [[ "${create_indexes}" == "0" ]]; then
    "${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X \
        -v ON_ERROR_STOP=1 -c 'VACUUM (ANALYZE);'
else
    echo "TPCH_CREATE_INDEXES must be 0 or 1" >&2
    exit 1
fi

"${psql_bin}" -h "${pg_host}" -p "${pg_port}" -d "${pg_database}" -X -qAt \
    -c "SELECT relname || E'\t' || n_live_tup::bigint FROM pg_stat_user_tables ORDER BY relname;"
