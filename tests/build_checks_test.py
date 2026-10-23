import importlib.util
import json
import subprocess
import sys
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

    def test_cli_explicit_local_paths(self):
        script = Path(__file__).resolve().parents[1] / 'scripts/check_build.py'
        subprocess.run([sys.executable, str(script), '--build', str(self.build),
                        '--config', str(self.config), '--partitions', str(self.partitions)],
                       cwd=self.root, check=True, capture_output=True, text=True)
        report = json.loads((self.build / 'quality-report.json').read_text())
        self.assertEqual(len(report['images']), 4)

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

    def test_dns_defaults_and_generated_config_gate(self):
        defaults = (Path(__file__).parents[1] / 'sdkconfig.defaults').read_text().splitlines()
        required = ('CONFIG_OPENTHREAD_DNS_CLIENT', 'CONFIG_OPENTHREAD_SRP_CLIENT',
                    'CONFIG_OPENTHREAD_DNS64_CLIENT',
                    'CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM',
                    'CONFIG_LWIP_HOOK_DNS_EXT_RESOLVE_NONE',
                    'CONFIG_LWIP_IPV4', 'CONFIG_LWIP_IPV6',
                    'CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS',
                    'CONFIG_ESP_TLS_INSECURE', 'CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY')
        for key in required:
            with self.subTest(config=key):
                self.assertIn(f'{key}={checks.EXPECTED_CONFIG[key]}', defaults)
        # A generated configuration predating the bridge must fail packaging.
        self.config.write_text(self.config.read_text().replace(
            'CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM=y',
            '# CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM is not set'))
        with self.assertRaisesRegex(ValueError, 'NETCONN_EXT_RESOLVE_CUSTOM'):
            self.check()

    def test_missing_ota_file(self):
        (self.build / 'ota_data_initial.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.check()

    def test_browser_upload_header_budget(self):
        # Representative browser POST metadata, without a firmware body or
        # real device address/credentials. This reproduces the old 512-byte
        # budget failure while leaving room for browser/version variation.
        headers = {
            'Host': 'unit-test.local',
            'Connection': 'keep-alive',
            'Content-Length': '1967312',
            'Content-Type': 'application/octet-stream',
            'X-Firmware-SHA256': 'a' * 64,
            'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
                          '(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36',
            'Accept': '*/*',
            'Accept-Language': 'en-AU,en-GB;q=0.9,en;q=0.8',
            'Accept-Encoding': 'gzip, deflate',
            'Origin': 'http://unit-test.local',
            'Referer': 'http://unit-test.local/',
            'Sec-Fetch-Dest': 'empty',
            'Sec-Fetch-Mode': 'cors',
            'Sec-Fetch-Site': 'same-origin',
        }
        wire = ('POST /api/firmware/upload HTTP/1.1\r\n' +
                ''.join(f'{key}: {value}\r\n' for key, value in headers.items()) + '\r\n').encode('ascii')
        limit = int(checks.EXPECTED_CONFIG['CONFIG_HTTPD_MAX_REQ_HDR_LEN'])
        self.assertGreater(len(wire), 512)
        self.assertLess(len(wire) + 1024, limit)
        self.assertLessEqual(limit, 4096)
        defaults = (Path(__file__).parents[1] / 'sdkconfig.defaults').read_text().splitlines()
        self.assertIn(f'CONFIG_HTTPD_MAX_REQ_HDR_LEN={limit}', defaults)
        # An old generated configuration must be rejected before packaging.
        self.config.write_text(self.config.read_text().replace(
            f'CONFIG_HTTPD_MAX_REQ_HDR_LEN={limit}', 'CONFIG_HTTPD_MAX_REQ_HDR_LEN=512'))
        with self.assertRaisesRegex(ValueError, 'HTTPD_MAX_REQ_HDR_LEN'):
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
