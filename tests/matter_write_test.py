import contextlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('spa_matter', ROOT / 'scripts/hardware/spa_matter.py')
matter = importlib.util.module_from_spec(spec);spec.loader.exec_module(matter)
spec = importlib.util.spec_from_file_location('matter_write', ROOT / 'scripts/hardware/matter_write.py')
write = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'spa_matter': matter}):spec.loader.exec_module(write)


class MatterWriteTests(unittest.TestCase):
    def test_allowlist_types_and_temperature_steps(self):
        self.assertEqual(write.parse_value('setpoint', '38.2'), 3820)
        self.assertIs(write.parse_value('on-off', 'off'), False)
        for prop, value in (('system-mode', '0'), ('acl', '1'), ('setpoint', '38.1'),
                            ('setpoint', 'NaN'), ('setpoint', '42'), ('percent', '101'),
                            ('on-off', 'toggle'), ('mode', '1; reboot'), ('speed', '-1')):
            with self.subTest(prop=prop, value=value), self.assertRaises(ValueError):write.parse_value(prop, value)

    def test_command_uses_writes_or_correct_cluster_commands(self):
        self.assertEqual(write.command('setpoint', 3820, 1, 44), ['any', 'write-by-id', '513', '18', '"s:3820"', '1', '44'])
        self.assertEqual(write.command('on-off', False, 1, 44), ['onoff', 'off', '1', '44'])
        self.assertEqual(write.command('mode', 4, 1, 44), ['modeselect', 'change-to-mode', '4', '1', '44'])

    def run_write(self, dry=False, rf_value=38.2, polls=11, fresh=True, outcome=None, advertised=True):
        temporary = tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        @contextlib.contextmanager
        def controller(req):yield directory, directory / 'controller', directory / 'chip-tool'
        values = iter([3800, 3820, 3820, 3820, 3820])
        def read(req, context):
            if req['cluster'] == 29:
                return {'status': 'supported', 'value': ([44] if advertised else []) if req['attribute'] == 3 else [29, 513]}
            if req['attribute'] == 0xfffb:return {'status': 'supported', 'value': [18]}
            return {'status': 'supported', 'value': next(values)}
        before = {'fresh': fresh, 'counters': {'polls_ok': 10}}
        after = {'fresh': True, 'counters': {'polls_ok': polls}, 'last_known_spa_state': {'setpoint_c': rf_value}}
        http_calls = []
        def http(url):
            http_calls.append(url)
            return before if len(http_calls) == 1 else after
        with patch.object(matter, 'controller', controller), patch.object(write.time, 'sleep'), patch.object(write.subprocess, 'run') as run:
            if isinstance(outcome, Exception):run.side_effect = outcome
            else:run.return_value = subprocess.CompletedProcess([], outcome or 0)
            result = write.operate(44, 'setpoint', '38.2', dry, 'http://test.invalid/api/diagnostics', 1, read, http)
            calls = run.call_args_list
        return result, calls

    def test_dry_run_discovers_sparse_endpoint_and_never_writes(self):
        result, calls = self.run_write(dry=True)
        self.assertEqual(result['status'], 'dry_run_complete')
        self.assertFalse(result['write_attempted'])
        self.assertEqual(calls, [])

    def test_one_write_uses_existing_storage_and_fresh_rf_confirmation(self):
        result, calls = self.run_write()
        self.assertTrue(result['accepted'] and result['rf_confirmed'])
        self.assertEqual(result['status'], 'confirmed')
        self.assertEqual(len(calls), 1)
        args = calls[0].args[0]
        self.assertEqual(args[1:3], ['any', 'write-by-id'])
        self.assertIn('--storage-directory', args)
        self.assertEqual(args[args.index('--commissioner-name') + 1], 'alpha')
        self.assertEqual(calls[0].kwargs['timeout'], 22)

    def test_optimistic_attribute_readback_is_not_rf_confirmation(self):
        for kwargs in ({'rf_value': 38.0}, {'polls': 10}):
            result, calls = self.run_write(**kwargs)
            self.assertTrue(result['accepted'] and result['readback_matches'])
            self.assertFalse(result['rf_confirmed'])
            self.assertEqual(result['status'], 'accepted_unverified')
            self.assertEqual(len(calls), 1)

    def test_stale_or_unadvertised_preflight_rejects_without_write(self):
        for kwargs in ({'fresh': False}, {'advertised': False}):
            with self.assertRaises(ValueError):self.run_write(**kwargs)

    def test_onoff_dry_run_checks_light_device_type_without_sending_command(self):
        with tempfile.TemporaryDirectory() as directory:
            @contextlib.contextmanager
            def controller(req):yield Path(directory), Path(directory), Path(directory) / 'chip-tool'
            for device_type in (0x100, 0x10):
                def read(req, context):
                    if req['cluster'] == 29:
                        return {'status': 'supported', 'value': {3: [44], 1: [29, 6], 0: [{'device_type': device_type}]}[req['attribute']]}
                    return {'status': 'supported', 'value': [0] if req['attribute'] == 0xfffb else False}
                with patch.object(matter, 'controller', controller), patch.object(write.subprocess, 'run') as run:
                    args = (44, 'on-off', 'off', True, '', 1, read, lambda url: None)
                    if device_type == 0x100:
                        self.assertEqual(write.operate(*args)['status'], 'dry_run_complete')
                    else:
                        with self.assertRaises(ValueError):write.operate(*args)
                    run.assert_not_called()

    def test_write_timeout_or_error_never_retries_and_reports_uncertainty(self):
        for outcome in (1, subprocess.TimeoutExpired('chip-tool', 22), OSError('private data')):
            result, calls = self.run_write(outcome=outcome)
            self.assertEqual(result['status'], 'uncertain')
            self.assertIsNone(result['accepted'])
            self.assertEqual(len(calls), 1)
            self.assertNotIn('private data', json.dumps(result))

    def test_native_fan_mapping_preserves_discrete_and_auto_modes(self):
        self.assertEqual(write.native_target('percent', 33, True, [1, 2, 11, 12, 13, 14, 15], 5), {12})
        self.assertEqual(write.native_target('percent', 25, False, [0, 2, 3, 4], 2), {3})
        self.assertEqual(write.native_target('fan-mode', 5, False, [0, 1, 4], 1), {4})
        self.assertEqual(write.native_target('fan-mode', 5, True, [1, 2, 11, 12, 13, 14, 15], 5), {1})
        with self.assertRaises(ValueError):write.native_target('fan-mode', 1, False, [0, 1], 1)

    def test_supported_modes_parser_extracts_only_native_mode_ids(self):
        req = matter.request(1, 44, 80, 2)
        lines = ['Endpoint: 44 Cluster: 0x0000_0050 Attribute 0x0000_0002 DataVersion: 1',
                 'SupportedModes: 2 entries', '[1]: {', 'Label: Off', 'Mode: 0', 'SemanticTags: 0 entries', '}',
                 '[2]: {', 'Label: Auto', 'Mode: 4', 'SemanticTags: 0 entries', '}']
        text = '\n'.join('[1] [TOO] ' + line for line in lines)
        self.assertEqual(matter.parse_report(text, req, 0)['value'], [0, 4])
        self.assertEqual(matter.parse_report(text.replace('Mode: 4', 'Mode: 0'), req, 0)['status'], 'failed')


if __name__ == '__main__':unittest.main()
