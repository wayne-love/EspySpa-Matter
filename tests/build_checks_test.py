import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('build_checks', Path(__file__).parents[1] / 'scripts/check_build.py')
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


class BuildChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.config = self.root / 'sdkconfig'
        self.config.write_text('\n'.join(f'{k}={v}' for k, v in checks.EXPECTED_CONFIG.items()))
        self.partitions = self.root / 'partitions.csv'
        self.partitions.write_text('ota_0,app,ota_0,0x20000,0x1E0000\nota_1,app,ota_1,0x200000,0x1E0000\n')
        self.files = {'0x0': 'bootloader/bootloader.bin', '0xC000': 'partition_table/partition-table.bin',
                      '0x1D000': 'ota_data_initial.bin', '0x20000': 'espyspa_matter.bin'}
        for name in self.files.values():
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'firmware fixture')
        self.metadata()

    def metadata(self):
        (self.build / 'flasher_args.json').write_text(json.dumps({'flash_files': self.files}))

    def check(self):
        return checks.check(self.build, self.config, self.partitions)

    def test_valid_package(self):
        self.assertEqual(len(self.check()['images']), 4)

    def test_empty_image(self):
        (self.build / 'espyspa_matter.bin').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'Empty'):
            self.check()

    def test_configuration_regressions(self):
        baseline = self.config.read_text()
        for key, value in checks.EXPECTED_CONFIG.items():
            with self.subTest(config=key):
                wrong = 'y' if value == 'n' else 'n'
                self.config.write_text(baseline.replace(f'{key}={value}', f'{key}={wrong}'))
                with self.assertRaisesRegex(ValueError, key):
                    self.check()
        self.config.write_text(baseline)

    def test_missing_ota_file(self):
        (self.build / 'ota_data_initial.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.check()

    def test_missing_ota_metadata(self):
        del self.files['0x1D000']
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'required images'):
            self.check()

    def test_wrong_console(self):
        self.config.write_text(self.config.read_text().replace('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y', 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n'))
        with self.assertRaisesRegex(ValueError, 'USB_SERIAL_JTAG'):
            self.check()

    def test_insufficient_ota_space(self):
        (self.build / 'espyspa_matter.bin').write_bytes(bytes(0x1E0000 - checks.MIN_OTA_FREE + 1))
        with self.assertRaisesRegex(ValueError, 'leave at least'):
            self.check()

    def test_overlap(self):
        (self.build / 'bootloader/bootloader.bin').write_bytes(bytes(0xC001))
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            self.check()

    def test_wrong_application_offset(self):
        self.files['0x30000'] = self.files.pop('0x20000')
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'offset'):
            self.check()

    def test_unsafe_path(self):
        self.files['0x400000'] = '../outside.bin'
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            self.check()


if __name__ == '__main__':
    unittest.main()
