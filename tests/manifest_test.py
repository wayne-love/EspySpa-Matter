import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "make_manifest", Path(__file__).parents[1] / "scripts/make_manifest.py"
)
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class ManifestTests(unittest.TestCase):
    def test_manifest_contains_binary_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "firmware.bin"
            image.write_bytes(b"ota-image")
            result = manifest.make_manifest(
                image, "1.2.3", "release", "abc1234",
                "https://example.invalid/espyspa.bin"
            )
        self.assertEqual(result["version"], "1.2.3")
        self.assertEqual(result["channel"], "release")
        self.assertEqual(result["target"], "esp32c6")
        self.assertEqual(result["bytes"], 9)
        self.assertEqual(result["sha256"], hashlib.sha256(b"ota-image").hexdigest())

    def test_rejects_bad_channel(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "firmware.bin"
            image.write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "channel"):
                manifest.make_manifest(
                    image, "1.0.0", "nightly", "abc",
                    "https://example.invalid/fw.bin"
                )

    def test_requires_https(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "firmware.bin"
            image.write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "https"):
                manifest.make_manifest(
                    image, "1.0.0", "release", "abc",
                    "http://example.invalid/fw.bin"
                )


if __name__ == "__main__":
    unittest.main()
