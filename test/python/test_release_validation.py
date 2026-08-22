import importlib.util
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY_ROOT / "scripts" / "validate_release.py"
SPEC = importlib.util.spec_from_file_location("validate_release", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
validate_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validate_release)


class ReleaseValidationTests(unittest.TestCase):
    def test_repository_metadata_is_consistent(self):
        version, errors = validate_release.validate(REPOSITORY_ROOT, "v0.1.2")
        self.assertEqual(version, "0.1.2")
        self.assertEqual(errors, [])

    def test_mismatched_tag_is_rejected(self):
        _, errors = validate_release.validate(REPOSITORY_ROOT, "v9.9.9")
        self.assertIn(
            "tag 'v9.9.9' does not match release v0.1.2",
            errors,
        )

    def test_upgrade_graph_can_span_multiple_versions(self):
        edges = {"0.1.0": {"0.1.1"}, "0.1.1": {"0.1.2"}}
        self.assertTrue(validate_release.upgrade_reaches("0.1.0", "0.1.2", edges))
        self.assertFalse(validate_release.upgrade_reaches("0.1.2", "0.1.0", edges))


if __name__ == "__main__":
    unittest.main()
