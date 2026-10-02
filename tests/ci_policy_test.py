import importlib.util
from pathlib import Path
import unittest
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location(
    "ci_policy", Path(__file__).resolve().parents[1] / "scripts" / "ci_policy.py"
)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class CiPolicyTests(unittest.TestCase):
    def test_documentation_only(self):
        self.assertFalse(policy.needs_firmware([
            "README.md", "LICENSE", "docs/BUILD.md", "docs/images/wiring.png"
        ]))

    def test_code_config_workflows_and_unknown_paths_require_build(self):
        for path in ["main/app_main.cpp", "main/idf_component.yml", "sdkconfig.defaults",
                     "partitions.csv", "CMakeLists.txt", "scripts/check_build.py",
                     ".github/workflows/validate.yml", "tests/protocol_test.cpp",
                     "AGENTS.md", "docs/generator.py", "docs/config.yml", "new-file"]:
            with self.subTest(path=path):
                self.assertTrue(policy.needs_firmware(["README.md", path]))

    def test_rename_out_of_source_requires_build(self):
        self.assertTrue(policy.needs_firmware(["main/old.cpp", "docs/old.md"]))

    def test_cli_checks_entire_diff_and_source_deletion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(["git", *args], cwd=root).decode().strip()
            git("init", "-q")
            git("config", "user.name", "CI test")
            git("config", "user.email", "ci@example.invalid")
            (root / "README.md").write_text("initial\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            (root / "README.md").write_text("docs\n")
            git("commit", "-qam", "docs")
            def classify(start):
                return subprocess.check_output([
                    sys.executable, str(Path(policy.__file__).resolve()), "--base", start
                ], cwd=root).decode().strip()
            self.assertEqual(classify(base), "firmware=false")
            (root / "source.cpp").write_text("int value;\n")
            git("add", ".")
            git("commit", "-qm", "code")
            with_source = git("rev-parse", "HEAD")
            (root / "README.md").write_text("later docs\n")
            git("commit", "-qam", "later docs")
            self.assertEqual(classify(base), "firmware=true")
            (root / "source.cpp").unlink()
            git("commit", "-qam", "remove code")
            self.assertEqual(classify(with_source), "firmware=true")

    def test_gate_accepts_successful_build_or_explicit_doc_skip(self):
        self.assertTrue(policy.gate_passes("success", "success", "success", "true", "success"))
        self.assertTrue(policy.gate_passes("success", "success", "success", "false", "skipped"))

    def test_gate_rejects_failures_cancellation_and_unexpected_skips(self):
        good = ["success", "success", "success", "true", "success"]
        for i in [0, 1, 2, 4]:
            for result in ["failure", "cancelled", "skipped", ""]:
                with self.subTest(index=i, result=result):
                    values = good.copy()
                    values[i] = result
                    self.assertFalse(policy.gate_passes(*values))
        for required in ["", "unknown", "false"]:
            self.assertFalse(policy.gate_passes("success", "success", "success", required, "success"))
        for result in ["failure", "cancelled"]:
            self.assertFalse(policy.gate_passes("success", "success", "success", "false", result))


if __name__ == "__main__":
    unittest.main()
