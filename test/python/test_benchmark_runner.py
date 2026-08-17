import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "run_pg_benchmark", ROOT / "scripts" / "run_pg_benchmark.py"
)
RUNNER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(RUNNER)


class QueryDiscoveryTests(unittest.TestCase):
    def test_reused_base_requires_identical_sql_hash(self):
        base = {"status": "ok"}
        record = {"query_sha256": "same", "base": base}
        self.assertIs(RUNNER.reusable_base(record, "same"), base)
        self.assertIsNone(RUNNER.reusable_base(record, "different"))
        self.assertIsNone(RUNNER.reusable_base(None, "same"))

    def test_flat_query_id_is_unchanged(self):
        root = Path("/queries")
        self.assertEqual(RUNNER.query_id(root, root / "q01.sql"), "q01")

    def test_recursive_query_id_is_collision_free(self):
        root = Path("/queries")
        self.assertEqual(
            RUNNER.query_id(root, root / "7a" / "abcdef.sql"),
            "7a__abcdef",
        )

    def test_evenly_spaced_selection_covers_endpoints(self):
        paths = [Path(f"q{index:02}.sql") for index in range(10)]
        selected = RUNNER.evenly_spaced(paths, 4)
        self.assertEqual(selected, [paths[0], paths[3], paths[6], paths[9]])

    def test_recursive_stratification_is_per_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for group in ("a", "b"):
                (root / group).mkdir()
                for index in range(5):
                    (root / group / f"q{index}.sql").write_text(
                        "SELECT 1;", encoding="utf-8"
                    )
            args = SimpleNamespace(
                queries_dir=root,
                recursive=True,
                queries_per_group=2,
                limit=None,
            )
            selected = RUNNER.discover_queries(args, RUNNER.re.compile(r".*"))
            self.assertEqual(len(selected), 4)
            self.assertEqual(
                [RUNNER.query_id(root, path) for path in selected],
                ["a__q0", "a__q4", "b__q0", "b__q4"],
            )


class OutputParsingTests(unittest.TestCase):
    def test_query_without_semicolon_is_terminated(self):
        self.assertEqual(RUNNER.terminate_sql("SELECT 1\n"), "SELECT 1\n;\n")

    def test_query_with_semicolon_is_not_changed(self):
        self.assertEqual(RUNNER.terminate_sql("SELECT 1;\n"), "SELECT 1;\n")

    def test_timing_splits_result_batches(self):
        timings, results = RUNNER.parse_psql_output(
            "b\na\nTime: 10.500 ms\nc\nTime: 8.250 ms\n"
        )
        self.assertEqual(timings, [10.5, 8.25])
        self.assertEqual(results, ["a\nb", "c"])

    def test_profile_is_out_of_band_from_timed_results(self):
        stdout = (
            "warm\nTime: 10.500 ms\n"
            "measured\nTime: 8.250 ms\n"
            f"{RUNNER.PROFILE_MARKER}\n"
            '{"status":"complete","planning_ms":2.0,'
            '"execution_ms":6.0,"total_ms":8.0,'
            '"planning":{"transfer_ms":1.0,"p1_ms":0.25},'
            '"executor":{"run_ms":5.5}}\n'
        )
        query_stdout, profile, error = RUNNER.split_profile_output(stdout)
        timings, results = RUNNER.parse_psql_output(query_stdout)
        self.assertIsNone(error)
        self.assertEqual(timings, [10.5, 8.25])
        self.assertEqual(results, ["warm", "measured"])
        self.assertEqual(profile["status"], "complete")
        self.assertEqual(
            RUNNER.profile_breakdown_ms(profile),
            {
                "planning": 2.0,
                "p0": None,
                "native_preview": None,
                "graph": None,
                "transfer": 1.0,
                "statistics": None,
                "p1": 0.25,
                "execution": 6.0,
                "executor_run": 5.5,
                "total": 8.0,
            },
        )

    def test_missing_profile_marker_is_reported(self):
        query_stdout, profile, error = RUNNER.split_profile_output(
            "value\nTime: 1.000 ms\n"
        )
        self.assertEqual(query_stdout, "value\nTime: 1.000 ms\n")
        self.assertIsNone(profile)
        self.assertEqual(error, "profile marker not found")

    def test_empty_profile_is_a_terminal_native_noop(self):
        self.assertTrue(RUNNER.profile_is_terminal({"status": "empty"}))
        self.assertTrue(RUNNER.profile_is_terminal({"status": "complete"}))
        self.assertFalse(RUNNER.profile_is_terminal({"status": "planned"}))


class SettingsTests(unittest.TestCase):
    def test_sampling_mode_is_explicit_in_pgoptions(self):
        args = SimpleNamespace(
            parallel_workers=8,
            transfer_workers=4,
            transfer_parallel_min_rows=100_000,
            index_transfer=True,
            index_transfer_max_fraction=0.15,
            index_transfer_batch_keys=65_536,
            index_transfer_max_keys=100_000,
            index_guard_min_rows=100_000,
            transfer_progress_metric="ndv",
            p1_parallel_bias=True,
            p1_parallel_min_rows=100_000,
            capture_profile=True,
            work_mem="512MB",
            hash_mem_multiplier=2.0,
            sample_mode="instant",
            sample_size=20_000,
            sample_seed=2,
            sample_rate=0.01,
        )

        options = RUNNER.pgoptions(args, True)

        self.assertIn("-c bloompg.sample_mode=instant", options)
        self.assertIn("-c bloompg.sample_size=20000", options)
        self.assertIn("-c bloompg.transfer_progress_metric=ndv", options)


if __name__ == "__main__":
    unittest.main()
