import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('spa_toolkit', Path(__file__).resolve().parents[1] / 'scripts/hardware/spa_toolkit.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
CFG = dict(ssh_host='pi.example.invalid', ssh_user='tester', ssh_key='/tmp/test-key',
           diagnostics_url='http://device.example.invalid:8080/api/diagnostics', log_path='/var/log/spa.log')
DATA = dict(firmware='test', fresh=True, thread={'role': 3}, matter={'fabric_count': 2}, counters={'polls_failed': 0})


class HardwareTests(unittest.TestCase):
    def setUp(self):
        home = tempfile.TemporaryDirectory()
        self.addCleanup(home.cleanup)
        self.home = Path(home.name)
        mocked_home = patch.object(tool.Path, 'home', return_value=self.home)
        mocked_home.start()
        self.addCleanup(mocked_home.stop)

    def write_env(self, content):
        path = self.home / '.config/espyspa/hardware.env'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def response(self, body, code=0):
        return subprocess.CompletedProcess([], code, body.encode(), b'sensitive stderr')

    @patch.object(tool.subprocess, 'run')
    def test_status_transport(self, run):
        run.return_value = self.response(json.dumps(DATA))
        self.assertEqual(tool.diagnostics(CFG), DATA)
        args, kwargs = run.call_args
        self.assertIn('StrictHostKeyChecking=yes', args[0])
        self.assertIn('/dev/null', args[0])
        self.assertIn('curl -q --globoff', args[0][-1])
        self.assertIn('--max-time 20', args[0][-1])
        self.assertNotIn('-L', args[0][-1])
        self.assertEqual(kwargs['timeout'], 30)

    @patch.object(tool.subprocess, 'run')
    def test_failures(self, run):
        for code in (255, 22, 28, 7, 63, 1):
            run.return_value = self.response('', code)
            with self.assertRaises(tool.DiagnosticError) as error:
                tool.diagnostics(CFG)
            self.assertNotIn('sensitive', str(error.exception))
        for exception in (subprocess.TimeoutExpired('ssh', 30), FileNotFoundError()):
            run.side_effect = exception
            with self.assertRaises(tool.DiagnosticError):
                tool.diagnostics(CFG)

    @patch.object(tool.subprocess, 'run')
    def test_invalid_http_and_size(self, run):
        for body in ('not json', '[]', '{"thread": null}', 'x' * (tool.LIMIT + 1)):
            run.return_value = self.response(body)
            with self.assertRaises(tool.DiagnosticError):
                tool.diagnostics(CFG)

    @patch.object(tool.subprocess, 'run')
    def test_reject_other_endpoints(self, run):
        for url in ([], {}, 123, 'http://host/reset', 'http://host:8081/api/diagnostics', 'http://user:pass@host/api/diagnostics', 'http://host/api/diagnostics?command=RF'):
            with self.assertRaises(tool.DiagnosticError):
                tool.diagnostics(dict(CFG, diagnostics_url=url))
        run.assert_not_called()

    @patch.object(tool.subprocess, 'run')
    def test_logs_quoted_and_bounded(self, run):
        run.return_value = self.response('log\n')
        self.assertEqual(tool.logs(dict(CFG, log_path='/tmp/a; touch nope'), 100), 'log\n')
        command = run.call_args.args[0][-1]
        self.assertIn('pipefail', command)
        self.assertIn('1048577', command)
        self.assertIn('tail -n 100', command)

    def test_health_and_redaction(self):
        self.assertTrue(tool.health(DATA)['healthy'])
        self.assertFalse(tool.health({})['healthy'])
        for count in (-1, True, '2', [], None):
            self.assertFalse(tool.health(dict(DATA, matter={'fabric_count': count}))['healthy'])
        self.assertFalse(tool.health(dict(DATA, fresh=False))['healthy'])
        value = tool.redact({'token': 'abc', 'log': '192.0.2.1 fd00::1 password=abc'})
        self.assertEqual(value['token'], '[redacted]')
        self.assertNotIn('abc', value['log'])
        self.assertNotIn('fd00::1', value['log'])

    def test_config_env_and_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'config.json'
            path.write_text(json.dumps(CFG))
            with patch.dict(tool.os.environ, {'ESPYSPA_SSH_USER': 'override'}, clear=True):
                self.assertEqual(tool.config(path)['ssh_user'], 'override')
            with patch.dict(tool.os.environ, {'ESPYSPA_SSH_HOST': '-oProxyCommand=bad'}, clear=True):
                with self.assertRaises(tool.DiagnosticError):
                    tool.config(path)

    @patch.object(tool, 'config', return_value=CFG)
    @patch.object(tool, 'logs', return_value='192.0.2.1 log')
    @patch.object(tool, 'diagnostics', return_value=DATA)
    def test_capture_and_cli(self, diagnostics, logs, config):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.main('spa-capture', ['--output-dir', directory]), 0)
            files = list(Path(directory).glob('spa-*.json'))
            self.assertEqual(len(files), 1)
            data = json.loads(files[0].read_text())
            self.assertIn('captured_at', data)
            self.assertNotIn('192.0.2.1', data['logs'])
            self.assertEqual(files[0].stat().st_mode & 0o777, 0o600)
            diagnostics.side_effect = tool.DiagnosticError('HTTP unavailable')
            self.assertEqual(tool.main('spa-capture', ['--output-dir', directory]), 1)
            self.assertEqual(len(list(Path(directory).glob('spa-*.json'))), 2)
        with contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(tool.main('spa-logs', ['--lines', '0']), 2)
            self.assertIn('error', json.loads(output.getvalue()))

    def test_spa_connection_configuration_strict(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'config.json'
            path.write_text(json.dumps(CFG))
            with patch.dict(tool.os.environ, {}, clear=True):
                self.assertTrue(tool.config(path)['spa_connected'])
            for value in ('true', 'false', '', 'TRUE', 'False', '0', '1', 'yes', ' false '):
                with self.subTest(value=value), patch.dict(tool.os.environ, {'ESPYSPA_SPA_CONNECTED': value}, clear=True):
                    if value in ('true', 'false'):
                        self.assertEqual(tool.config(path)['spa_connected'], value == 'true')
                    else:
                        with contextlib.redirect_stderr(io.StringIO()) as output, patch.object(tool, 'ssh') as ssh:
                            self.assertEqual(tool.main('spa-health', ['--config', str(path)]), 2)
                            self.assertIn('ESPYSPA_SPA_CONNECTED must be exactly true or false', json.loads(output.getvalue())['error'])
                            ssh.assert_not_called()

    @patch.object(tool, 'config')
    @patch.object(tool, 'logs', return_value='[2026-10-21T00:00:00Z] serial output')
    @patch.object(tool, 'diagnostics')
    def test_connected_and_disconnected_health_cli(self, diagnostics, logs, config):
        stale = dict(DATA, fresh=False, last_error='missing RF header', raw_rf='',
                     transactions=[{'tx': 'RF', 'rx': '', 'result': 'missing RF header'}],
                     counters={'polls_failed': 193, 'polls_ok': 0})
        for connected in (True, False):
            config.return_value = dict(CFG, spa_connected=connected)
            diagnostics.return_value = stale
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(tool.main('spa-health', ['--json']), 1 if connected else 0)
            result = json.loads(output.getvalue())
            self.assertEqual(result['diagnostics'], stale)
            self.assertEqual(result['spa_connected'], connected)
            self.assertTrue(result['serial_log']['readable'])
            if not connected:
                self.assertEqual(result['spa_status'], 'intentionally_disconnected')
            for change in ({'thread': {'role': 1}}, {'matter': {'fabric_count': 0}}):
                diagnostics.return_value = dict(stale, **change)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(tool.main('spa-health', ['--json']), 1)
            diagnostics.return_value = stale
            for failure in (tool.DiagnosticError('SSH failed'), tool.DiagnosticError('Serial read failed')):
                logs.side_effect = failure
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(tool.main('spa-health', ['--json']), 1)
                self.assertFalse(json.loads(output.getvalue())['serial_log']['readable'])
            logs.side_effect = None
            diagnostics.side_effect = tool.DiagnosticError('Remote HTTP timeout')
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(tool.main('spa-health', ['--json']), 2)
            diagnostics.side_effect = None

    @patch.object(tool, 'config', return_value=dict(CFG, spa_connected=False))
    @patch.object(tool, 'logs', return_value='serial output')
    @patch.object(tool, 'diagnostics', return_value=dict(DATA, fresh=False))
    def test_capture_disconnected_policy_and_serial_failure(self, diagnostics, logs, config):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.main('spa-capture', ['--output-dir', directory]), 0)
            capture = json.loads(next(Path(directory).glob('*.json')).read_text())
            self.assertTrue(capture['health']['healthy'])
            self.assertEqual(capture['health']['spa_status'], 'intentionally_disconnected')
            self.assertFalse(capture['diagnostics']['fresh'])
            logs.side_effect = tool.DiagnosticError('Serial unavailable')
            self.assertEqual(tool.main('spa-capture', ['--output-dir', directory]), 1)
            partial = [json.loads(p.read_text()) for p in Path(directory).glob('*.json') if json.loads(p.read_text())['errors']][0]
            self.assertFalse(partial['health']['healthy'])

    @patch.object(tool, 'config', return_value=CFG)
    @patch.object(tool, 'logs', return_value='serial output')
    @patch.object(tool, 'diagnostics')
    def test_health_summary_modes_and_json_compatibility(self, diagnostics, logs, config):
        complete = dict(DATA, uptime_ms=90061000, free_heap=214472)
        cases = [
            (complete, True, 0, ['Overall health: Healthy', 'Firmware: test',
                               'Uptime: 1d 01h 01m 01s', 'Serial log: Readable',
                               'Thread role: Router', 'Matter fabrics: 2',
                               'Free heap: 214,472 bytes', 'Spa: Expected connected']),
            (dict(complete, fresh=False, thread={'role': 1}, matter={'fabric_count': 0}), True, 1,
             ['Overall health: Unhealthy', 'check the spa connection', 'check the border router', 'check commissioning status']),
            (dict(complete, fresh=False, counters={'polls_failed': 193}), False, 0,
             ['Overall health: Healthy', 'Spa: Intentionally disconnected']),
            ({}, True, 1, ['Firmware: Unknown', 'Uptime: Unknown', 'Thread role: Unknown',
                          'Matter fabrics: Unknown', 'Free heap: Unknown']),
        ]
        for data, connected, code, expected in cases:
            with self.subTest(data=data, connected=connected):
                config.return_value = dict(CFG, spa_connected=connected)
                diagnostics.return_value = data
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(tool.main('spa-health', ['--summary']), code)
                for phrase in expected:
                    self.assertIn(phrase, output.getvalue())
                if not connected:
                    self.assertNotIn('Issue:', output.getvalue())
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(tool.main('spa-health', ['--json']), code)
                self.assertEqual(json.loads(output.getvalue()), tool.redact(tool.health(data, connected)))
        logs.side_effect = tool.DiagnosticError('read failed')
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(tool.main('spa-health', ['--summary']), 1)
        self.assertIn('Serial log: Unavailable', output.getvalue())
        self.assertIn('read permissions', output.getvalue())

    def test_summary_missing_data_and_exclusive_options(self):
        summary = tool.health_summary({})
        self.assertIn('Overall health: Unknown', summary)
        self.assertIn('Serial log: Unknown', summary)
        self.assertIn('Spa: Unknown', summary)
        with patch.object(tool, 'config') as config, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                tool.main('spa-health', ['--summary', '--json'])
            self.assertEqual(error.exception.code, 2)
            config.assert_not_called()

    def test_home_env_parsing_and_precedence(self):
        self.write_env("# defaults\nexport ESPYSPA_SSH_HOST=pi.example.invalid\n"
                       "ESPYSPA_SSH_USER='file-user' # comment\n"
                       'ESPYSPA_SSH_KEY="/tmp/key with spaces"\n'
                       "ESPYSPA_DIAGNOSTICS_URL=http://device.example.invalid:8080/api/diagnostics\n"
                       "ESPYSPA_SPA_CONNECTED=false\n")
        path = self.home / 'local.json'
        with patch.dict(tool.os.environ, {}, clear=True):
            result = tool.config(path)
            self.assertEqual(result['ssh_user'], 'file-user')
            self.assertEqual(result['ssh_key'], '/tmp/key with spaces')
            self.assertFalse(result['spa_connected'])
            path.write_text(json.dumps({'ssh_user': 'json-user'}))
            self.assertEqual(tool.config(path)['ssh_user'], 'json-user')
            with patch.dict(tool.os.environ, {'ESPYSPA_SSH_USER': 'env-user', 'ESPYSPA_SPA_CONNECTED': 'true'}):
                self.assertEqual(tool.config(path)['ssh_user'], 'env-user')
                self.assertTrue(tool.config(path)['spa_connected'])
            with patch.dict(tool.os.environ, {'ESPYSPA_SSH_USER': ''}):
                with self.assertRaises(tool.DiagnosticError):
                    tool.config(path)

    def test_home_env_rejects_unsupported_without_exposure(self):
        bad = ["SECRET=private-value", "source private-value", "ESPYSPA_SSH_KEY=$(private-value)",
               "ESPYSPA_SSH_KEY=`private-value`", "ESPYSPA_SSH_KEY='private-value",
               "ESPYSPA_SSH_KEY=private-value;command", 'ESPYSPA_SSH_KEY="private-value" extra',
               "ESPYSPA_SSH_KEY=private-value\nESPYSPA_SSH_KEY=duplicate",
               "ESPYSPA_SPA_CONNECTED=FALSE", "ESPYSPA_SSH_KEY=private-value\x00"]
        for content in bad:
            with self.subTest(content=content):
                self.write_env(content)
                with patch.object(tool.subprocess, 'run') as run:
                    with self.assertRaises(tool.DiagnosticError) as error:
                        tool.hardware_env()
                    self.assertIn('hardware.env line', str(error.exception))
                    self.assertNotIn('private-value', str(error.exception))
                    run.assert_not_called()

    def test_home_env_missing_unreadable_and_size(self):
        self.assertEqual(tool.hardware_env(), {})
        path = self.write_env('x' * 65537)
        with self.assertRaisesRegex(tool.DiagnosticError, 'limit'):
            tool.hardware_env()
        path.write_bytes(b'\xff')
        with self.assertRaisesRegex(tool.DiagnosticError, 'UTF-8'):
            tool.hardware_env()
        with patch.object(tool.Path, 'open', side_effect=PermissionError('sensitive path')):
            with self.assertRaisesRegex(tool.DiagnosticError, '^Cannot read hardware.env'):
                tool.hardware_env()
