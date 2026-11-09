"""PG integration: worker-private filter union versus leader and native results.
Run against a disposable database with bloompg installed (PGHOST/PGPORT/PGDATABASE).
"""
import json
import os
import subprocess

PSQL = os.environ.get('PSQL', 'psql')

def sql(text):
    p = subprocess.run([PSQL, '-XqAt', '-v', 'ON_ERROR_STOP=1'],
                       input=text, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(p.stderr + '\n' + p.stdout[-3000:])
    return p.stdout.strip().splitlines()

SETTINGS = '''
SET search_path = worker_build_test, public;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SET bloompg.index_transfer = off;
SET bloompg.sample_mode = 'prepared';
SET bloompg.sample_seed = 2;
SET bloompg.materialization_memory = '512MB';
SET bloompg.transfer_workers = 4;
SET bloompg.transfer_parallel_min_rows = 0;
SET max_parallel_workers_per_gather = 4;
SET max_parallel_workers = 8;
SET jit = off;
SET work_mem = '64MB';
'''

def run(query, mode, extra=''):
    lines = sql(SETTINGS + extra + '\n' +
                f"SET bloompg.enable = {'off' if mode == 'native' else 'on'};\n" +
                f"SET bloompg.parallel_filter_build = {'on' if mode == 'worker' else 'off'};\n" +
                query + ';\nSELECT bloompg_last_profile()::text;')
    return lines[0], json.loads(lines[1])

def totals(profile):
    mats = [r['materialization'] for r in
            profile.get('transfer', {}).get('relations', [])]
    return {k: sum(m.get(k, 0) for m in mats) for k in
            ['worker_filter_parts', 'worker_filter_bytes',
             'worker_filter_build_ms', 'worker_filter_merge_ms',
             'filter_finalize_rescans', 'one_pass_bitmap_builds']}

sql('''
SET bloompg.enable = off;
CREATE SCHEMA worker_build_test;
CREATE TABLE worker_build_test.l AS
SELECT CASE WHEN g % 97 = 0 THEN NULL ELSE g - 200001 END AS k,
       CASE WHEN g % 89 = 0 THEN NULL ELSE g::bigint * 10000000000 END AS sparse,
       CASE WHEN g % 83 = 0 THEN NULL ELSE 'key-' || g END AS txt,
       g % 3 = 0 AS keep, NULL::integer AS nul
FROM generate_series(1, 400000) g;
CREATE TABLE worker_build_test.r AS
SELECT CASE WHEN g % 97 = 0 THEN NULL ELSE g - 200001 END AS k,
       CASE WHEN g % 89 = 0 THEN NULL ELSE g::bigint * 10000000000 END AS sparse,
       CASE WHEN g % 83 = 0 THEN NULL ELSE 'key-' || g END AS txt,
       NULL::integer AS nul
FROM generate_series(1, 600000) g;
ANALYZE worker_build_test.l, worker_build_test.r;
''')
cases = {
    'signed_bitmap': 'SELECT count(*), sum(l.k::bigint) FROM l JOIN r USING(k) WHERE l.keep',
    'text_bloom': 'SELECT count(*), sum(l.k::bigint) FROM l JOIN r USING(txt) WHERE l.keep',
    'composite_bloom': 'SELECT count(*) FROM l JOIN r USING(k,txt) WHERE l.keep',
    'sparse_fallback': 'SELECT count(*) FROM l JOIN r USING(sparse) WHERE l.keep',
    'shared_key': 'SELECT count(*) FROM l JOIN r x ON l.k=x.k JOIN r y ON l.k=y.k WHERE l.keep',
    'mixed_keys': 'SELECT count(*) FROM l JOIN r x ON l.k=x.k JOIN r y ON l.sparse=y.sparse WHERE l.keep',
    'null_key': 'SELECT count(*) FROM l JOIN r USING(nul) WHERE l.keep',
    'empty_source': 'SELECT count(*) FROM l JOIN r USING(k) WHERE l.keep AND l.k < -200001',
}
results = []
try:
    for name, query in cases.items():
        native, _ = run(query, 'native')
        for participation in ['on', 'off']:
            extra = f'SET parallel_leader_participation = {participation};'
            leader, _ = run(query, 'leader', extra)
            worker, p = run(query, 'worker', extra)
            assert native == leader == worker, (name, native, leader, worker)
            t = totals(p)
            assert p['planning']['replanned'] and not p['planning']['transfer_failed'], (name, p)
            # Empty/null joins may be optimized away or conclude before a build.
            if name not in ['null_key', 'empty_source']:
                assert t['worker_filter_parts'] > 0, (name, p)
            if name == 'signed_bitmap':
                assert t['one_pass_bitmap_builds'] > 0, t
                assert t['filter_finalize_rescans'] == 0, t
            if name == 'sparse_fallback':
                assert t['filter_finalize_rescans'] > 0, t
            result = dict(case=name, leader_participation=participation,
                          result=worker, **t)
            results.append(result)
            print(json.dumps(result), flush=True)
    query = cases['signed_bitmap']
    worker, p = run(query, 'worker', "SET bloompg.materialization_memory = '160MB';")
    native, _ = run(query, 'native')
    assert worker == native
    assert totals(p)['worker_filter_parts'] == 0, p
    assert any(r['materialization']['parallel']['scans'] > 0
               for r in p['transfer']['relations']), p
    print('PASS: memory-budget fallback retains parallel scanning')
    worker, p = run(query, 'worker', 'SET max_parallel_workers_per_gather = 0;')
    assert worker == native and totals(p)['worker_filter_parts'] == 0
    print('PASS: serial execution unchanged')
finally:
    sql('SET bloompg.enable=off; DROP SCHEMA worker_build_test CASCADE;')
