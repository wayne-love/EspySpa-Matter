import importlib.util
import io
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('spa_matter', Path(__file__).resolve().parents[1] / 'scripts/hardware/spa_matter.py')
matter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matter)
pair_spec = importlib.util.spec_from_file_location('pi_pair', Path(__file__).resolve().parents[1] / 'scripts/hardware/pi_pair.py')
pairing = importlib.util.module_from_spec(pair_spec)
with patch.dict(sys.modules, {'spa_matter': matter}):
    pair_spec.loader.exec_module(pairing)


def report(req, *lines):
    header = f"Endpoint: {req['endpoint']} Cluster: 0x{req['cluster']:08X} Attribute 0x{req['attribute']:08X} DataVersion: 42"
    return '\n'.join('[100.000000][1:2] CHIP:TOO: ' + line for line in (header,) + lines)


class MatterToolkitTests(unittest.TestCase):
    def controller_home(self, directory, node=1):
        home = Path(directory)
        state = home / '.local/state/espyspa-matter'
        state.mkdir(parents=True, mode=0o700)
        (state / 'controller').mkdir(mode=0o700)
        metadata = state / 'commissioned.json'
        metadata.write_text(json.dumps({'node_id': node, 'revision': matter.REVISION}))
        metadata.chmod(0o600)
        return home

    def test_allowlist_and_node_validation(self):
        req = matter.request('0x45535041', 12, 0x202, 5)
        self.assertEqual(req['node'], 0x45535041)
        for args in ((0, 0, 29, 3), (True, 0, 29, 3), ('1; reboot', 0, 29, 3),
                     (1, 65535, 29, 3), (1, 0, 0x3e, 0), (1, 0, 6, 1)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                matter.request(*args)

    def test_scalars_and_null(self):
        for cluster, attribute, label, value, expected in (
            (0x201, 0, 'LocalTemperature', 'null', None),
            (0x201, 0, 'LocalTemperature', '-120', -120),
            (6, 0, 'OnOff', 'TRUE', True),
            (0x28, 3, 'ProductName', 'eSpa', 'eSpa'),
        ):
            req = matter.request(1, 9, cluster, attribute)
            self.assertEqual(matter.parse_report(report(req, f'{label}: {value}'), req, 0),
                             {'status': 'supported', 'value': expected})
            alternate = report(req, f'{label}: {value}').replace('CHIP:TOO:', 'CHIP: [TOO]')
            self.assertEqual(matter.parse_report(alternate, req, 0)['value'], expected)

    def test_descriptor_lists_and_device_types(self):
        req = matter.request(1, 0, 29, 3)
        self.assertEqual(matter.parse_report(report(req, 'PartsList: 2 entries', '[1]: 12', '[2]: 44'), req, 0)['value'], [12, 44])
        req['attribute'] = 1
        self.assertEqual(matter.parse_report(report(req, 'ServerList: 1 entries', '[1]: 513 (Thermostat)'), req, 0)['value'], [513])
        req['attribute'] = 0
        value = matter.parse_report(report(req, 'DeviceTypeList: 1 entries', '[1]: {',
                                          'DeviceType: 769 (Thermostat)', 'Revision: 3', '}'), req, 0)
        self.assertEqual(value['value'], [{'device_type': 769, 'revision': 3}])

    def test_live_stdio_color_and_mei_separator_format(self):
        req = matter.request(1, 0, 29, 3)
        text = '\n'.join('\x1b[0;32m[100.123456] [1:2] [TOO] ' + line + '\x1b[0m' for line in (
            'Endpoint: 0 Cluster: 0x0000_001D Attribute 0x0000_0003 DataVersion: 2501940578',
            '   PartsList: 2 entries', '     [1]: 12', '     [2]: 44'))
        self.assertEqual(matter.parse_report(text, req, 0), {'status': 'supported', 'value': [12, 44]})

    def test_missing_truncated_mismatched_and_duplicate_reports_fail(self):
        req = matter.request(1, 0, 29, 3)
        for text in ('PartsList: 1 entries',
                     report(dict(req, endpoint=7), 'PartsList: 0 entries'),
                     report(req, 'PartsList: 2 entries', '[1]: 12'),
                     report(req, 'PartsList: 1 entries', '[2]: 12'),
                     report(req, 'PartsList: 2 entries', '[1]: 12', '[1]: 12'),
                     report(req, 'PartsList: null'),
                     report(req, 'PartsList: 600 entries'),
                     report(req, 'PartsList: 0 entries', 'List truncated due to invalid value')):
            with self.subTest(text=text):
                self.assertEqual(matter.parse_report(text, req, 0)['status'], 'failed')

    def test_errors_never_echo_sensitive_output(self):
        req = matter.request(1, 0, 29, 3)
        for code in (1, -9, 255):
            parsed = matter.parse_report('secret=NEVER_PRINT pin=12345678', req, code)
            self.assertEqual(parsed['status'], 'failed')
            self.assertNotIn('NEVER_PRINT', str(parsed))
        parsed = matter.parse_report('UnsupportedAttribute secret=NEVER_PRINT', req, 1)
        self.assertEqual(parsed['status'], 'unsupported')
        self.assertNotIn('NEVER_PRINT', str(parsed))
        with self.assertRaises(ValueError):
            matter.parse_report('x' * (matter.LIMIT + 1), req, 0)

    def test_snapshot_discovers_sparse_ids_and_skips_unadvertised_attributes(self):
        calls = []

        def read(req):
            calls.append(req)
            ep, cl, attr = (req[k] for k in ('endpoint', 'cluster', 'attribute'))
            if cl == 29:
                value = {3: [44] if ep == 0 else [], 0: [{'device_type': 1, 'revision': 1}],
                         1: [29] if ep == 0 else [29, 6]}[attr]
            else:
                value = [0] if attr == 0xfffb else True
            return {'status': 'supported', 'value': value}

        result = matter.snapshot(1, read)
        self.assertEqual(result['endpoint_ids'], [0, 44])
        self.assertTrue(result['complete'])
        self.assertEqual([(r['cluster'], r['attribute']) for r in calls if r['endpoint'] == 44][-2:], [(6, 0xfffb), (6, 0)])
        self.assertFalse(any(r['endpoint'] in (1, 2, 3, 8) for r in calls))

    def test_unadvertised_attributes_separate_from_failures(self):
        def read(req):
            if req['cluster'] == 29:
                return {'status': 'supported', 'value': {3: [], 0: [], 1: [29, 6]}[req['attribute']]}
            return {'status': 'supported', 'value': []}
        result = matter.snapshot(1, read)
        self.assertEqual(len(result['unsupported']), 1)
        self.assertEqual(result['failed'], [])
        self.assertTrue(result['complete'])
        result = matter.snapshot(1, lambda req: {'status': 'failed', 'reason': 'offline'})
        self.assertFalse(result['complete'])
        self.assertEqual(len(result['failed']), 3)

    def test_topology_is_bounded(self):
        def read(req):
            value = {3: list(range(1, 18)), 0: [], 1: [29]}[req['attribute']]
            return {'status': 'supported', 'value': value}
        with self.assertRaisesRegex(ValueError, '16-endpoint'):
            matter.snapshot(1, read)

    def test_storage_modes_ownership_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'record'
            path.write_text('{}')
            path.chmod(0o600)
            matter.private_path(path)
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                matter.private_path(path)
            link = Path(directory) / 'link'
            link.symlink_to(path)
            with self.assertRaises(ValueError):
                matter.private_path(link)

    def test_remote_requires_commissioning_record_before_subprocess(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(matter.Path, 'home', return_value=Path(directory)), patch.object(matter.subprocess, 'run') as run:
            with self.assertRaises(FileNotFoundError):
                matter.remote_read(matter.request(1, 0, 29, 3))
            run.assert_not_called()

    def test_remote_uses_same_private_storage_and_bounded_read_only_argv(self):
        req = matter.request(1, 12, 6, 0)

        def run(args, **kwargs):
            self.assertEqual(args[1:3], ['any', 'read-by-id'])
            self.assertIn('--storage-directory', args)
            self.assertTrue(args[args.index('--storage-directory') + 1].endswith('/.local/state/espyspa-matter/controller'))
            self.assertEqual(args[args.index('--commissioner-name') + 1], 'alpha')
            self.assertEqual(kwargs['timeout'], 22)
            self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)
            self.assertIs(kwargs['preexec_fn'], matter.bounded_child)
            kwargs['stdout'].write(report(req, 'OnOff: TRUE').encode())
            return subprocess.CompletedProcess(args, 0)

        with tempfile.TemporaryDirectory() as directory:
            home = self.controller_home(directory)
            with patch.object(matter.Path, 'home', return_value=home), patch.object(matter.subprocess, 'run', side_effect=run) as mocked:
                self.assertTrue(matter.remote_read(req)['value'])
                with self.assertRaises(ValueError):
                    matter.remote_read(dict(req, node=2))
                self.assertEqual(mocked.call_count, 1)
            with patch.object(matter.Path, 'home', return_value=home), patch.object(matter.subprocess, 'run', side_effect=subprocess.TimeoutExpired('chip-tool', 22)):
                self.assertEqual(matter.remote_read(req)['status'], 'failed')

    def test_subscription_is_bounded_and_explicitly_shut_down(self):
        req = matter.request(1, 12, 0x201, 0)

        class Process:
            def __init__(self, args, **kwargs):
                self.stdin = io.BytesIO()
                self.returncode = 0
                self.args = args
                self.cleanup = None
                kwargs['stdout'].write((report(req, 'LocalTemperature: 3700') + '\nSubscription established in 10ms').encode())

            def communicate(self, data=None, timeout=None):
                self.cleanup = data
                self.timeout = timeout
                return None, None

            def poll(self):
                return self.returncode

        with tempfile.TemporaryDirectory() as directory:
            home = self.controller_home(directory)
            process = None

            def popen(args, **kwargs):
                nonlocal process
                process = Process(args, **kwargs)
                return process

            with patch.object(matter.Path, 'home', return_value=home), patch.object(matter.subprocess, 'Popen', side_effect=popen), patch.object(matter.time, 'sleep') as sleep:
                result = matter.remote_subscribe(req)
            self.assertEqual(result['status'], 'supported')
            self.assertTrue(result['established'])
            self.assertTrue(result['clean_termination'])
            sleep.assert_called_once_with(12)
            self.assertEqual(process.args[1:3], ['interactive', 'start'])
            self.assertEqual(process.cleanup, b'subscriptions shutdown-all\nquit\n')
            self.assertEqual(process.timeout, 10)
            self.assertIn(b'--keepSubscriptions 1 --auto-resubscribe 0 --timeout 8', process.stdin.getvalue())

    def pairing_home(self, directory):
        home = Path(directory)
        state = home / '.local/state/espyspa-matter'
        (state / 'controller').mkdir(parents=True, mode=0o700)
        state.chmod(0o700)
        install = home / '.local/lib/espyspa-matter'
        install.mkdir(parents=True)
        (install / 'chip-tool').touch()
        (install / 'paa-root-certs').mkdir()
        return home, state

    def test_pairing_single_attempt_ip_only_and_secret_output_suppressed(self):
        with tempfile.TemporaryDirectory() as directory:
            home, state = self.pairing_home(directory)
            with patch.object(pairing.Path, 'home', return_value=home), patch.object(pairing, 'capture', return_value=(1, 'secret=NEVER_PRINT')) as capture:
                result = pairing.pair('1' * 11, state)
                self.assertFalse(result['commissioned'])
                self.assertNotIn('NEVER_PRINT', str(result))
                args = capture.call_args.args[0]
                self.assertIn('--use-only-onnetwork-discovery', args)
                self.assertEqual(args[args.index('--use-only-onnetwork-discovery') + 1], '1')
                self.assertNotIn('--bypass-attestation-verifier', args)
                with self.assertRaises(FileExistsError):
                    pairing.pair('1' * 11, state)
                self.assertEqual(capture.call_count, 1)
                self.assertFalse((state / 'commissioned.json').exists())

    def test_pairing_requires_success_and_case_readback_before_marker(self):
        req = matter.request(pairing.NODE_ID, 0, 29, 3)
        for read_status in (0, 1):
            with self.subTest(read_status=read_status), tempfile.TemporaryDirectory() as directory:
                home, state = self.pairing_home(directory)
                with patch.object(pairing.Path, 'home', return_value=home), patch.object(pairing, 'capture', side_effect=[
                    (0, 'Device commissioning completed with success'),
                    (read_status, report(req, 'PartsList: 1 entries', '[1]: 44'))]):
                    result = pairing.pair('1' * 11, state)
                self.assertTrue(result['commissioned'])
                self.assertEqual(result['verified'], read_status == 0)
                self.assertEqual((state / 'commissioned.json').exists(), read_status == 0)
                if read_status == 0:
                    self.assertEqual((state / 'commissioned.json').stat().st_mode & 0o777, 0o600)

    def test_recovery_verifies_existing_identity_without_recommissioning(self):
        req = matter.request(pairing.NODE_ID, 0, 29, 3)
        with tempfile.TemporaryDirectory() as directory:
            home, state = self.pairing_home(directory)
            attempt = state / 'pairing-attempt.json'
            attempt.write_text(json.dumps({'node_id': pairing.NODE_ID, 'revision': matter.REVISION}))
            attempt.chmod(0o600)

            def capture(args, *unused):
                self.assertEqual(args[1:3], ['any', 'read-by-id'])
                self.assertNotIn('pairing', args)
                return 0, report(req, 'PartsList: 1 entries', '[1]: 44')

            with patch.object(pairing.Path, 'home', return_value=home), patch.object(pairing, 'capture', side_effect=capture):
                self.assertTrue(pairing.verify_existing(state)['verified'])
                metadata = state / 'commissioned.json'
                before = metadata.stat().st_mtime_ns
                self.assertTrue(pairing.verify_existing(state)['verified'])
                self.assertEqual(metadata.stat().st_mtime_ns, before)

    def test_pairing_rejects_original_pin_format_and_non_terminal(self):
        for code in ('1' * 8, 'MT:do-not-echo', '1; reboot', ''):
            with self.assertRaises(ValueError):
                pairing.fresh_code(code)
        with patch.object(pairing.sys.stdin, 'isatty', return_value=False), patch.object(pairing.getpass, 'getpass') as prompt, patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(pairing.main(), 2)
            prompt.assert_not_called()


if __name__ == '__main__':
    unittest.main()
