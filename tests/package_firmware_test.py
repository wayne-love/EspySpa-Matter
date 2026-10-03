import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "package_firmware", Path(__file__).parents[1] / "scripts/package_firmware.py"
)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackageFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / "build"
        self.build.mkdir()
        (self.build / packager.INTERNAL_APP_NAME).write_bytes(b"firmware")
        self.metadata = {
            "flash_files": {
                "0x0": "bootloader/bootloader.bin",
                "0x20000": packager.INTERNAL_APP_NAME,
            },
            "app": {
                "offset": "0x20000",
                "file": packager.INTERNAL_APP_NAME,
            },
        }
        self.write_metadata()

    def write_metadata(self):
        (self.build / "flasher_args.json").write_text(json.dumps(self.metadata))

    def test_packages_single_public_image_and_rewrites_metadata(self):
        packager.package_firmware(self.build)

        public = self.build / packager.PUBLIC_APP_NAME
        self.assertEqual(public.read_bytes(), b"firmware")

        packaged = json.loads((self.build / "flasher_args.json").read_text())
        self.assertEqual(packaged["flash_files"]["0x20000"], packager.PUBLIC_APP_NAME)
        self.assertEqual(packaged["app"]["file"], packager.PUBLIC_APP_NAME)
        self.assertNotIn(packager.INTERNAL_APP_NAME, packaged["flash_files"].values())

    def test_missing_app_metadata_fails(self):
        del self.metadata["app"]
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "missing app.file"):
            packager.package_firmware(self.build)

    def test_mismatched_app_file_fails(self):
        self.metadata["app"]["file"] = "other.bin"
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "Expected app.file"):
            packager.package_firmware(self.build)

    def test_mismatched_app_offset_fails(self):
        self.metadata["app"]["offset"] = "0x30000"
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "does not match"):
            packager.package_firmware(self.build)

    def test_missing_internal_image_fails(self):
        (self.build / packager.INTERNAL_APP_NAME).unlink()
        with self.assertRaisesRegex(ValueError, "Missing or empty application image"):
            packager.package_firmware(self.build)

    def test_missing_internal_metadata_reference_fails(self):
        self.metadata["flash_files"]["0x20000"] = "other.bin"
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            packager.package_firmware(self.build)

    def test_duplicate_internal_metadata_reference_fails(self):
        self.metadata["flash_files"]["0x30000"] = packager.INTERNAL_APP_NAME
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            packager.package_firmware(self.build)


if __name__ == "__main__":
    unittest.main()
