import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "bloompg_tune", ROOT / "scripts" / "bloompg_tune.py"
)
TUNER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(TUNER)


class TuningTests(unittest.TestCase):
    def test_rounding_quantum_never_increases_small_target(self):
        self.assertEqual(TUNER.floor_to(128 * TUNER.MIB, TUNER.GIB), 128 * TUNER.MIB)

    def test_query_budget_is_shared_by_concurrent_queries(self):
        result = TUNER.recommend(
            memory=4 * TUNER.GIB,
            cpus=16,
            concurrency=4,
            active_operators=8,
            storage="ssd",
        )
        self.assertEqual(
            result["bloompg"]["bloompg.materialization_memory"], "512MB"
        )


if __name__ == "__main__":
    unittest.main()
