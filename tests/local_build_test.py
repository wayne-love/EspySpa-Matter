"""Host regressions for local build orchestration; no Docker daemon required."""
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('local_commands', ROOT / 'tools/docker/commands.py')
commands = importlib.util.module_from_spec(spec)
spec.loader.exec_module(commands)


class LocalBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name)
        for name, value in {'STATE': self.state, 'BUILD': self.state / 'build',
                            'CONFIG': self.state / 'sdkconfig'}.items():
            p = patch.object(commands, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.current = {'version': '0.1.0-dev.local+1234567-dirty', 'channel': 'development',
                        'commit': '1234567-dirty', 'revision': '1234567' + '0' * 33, 'dirty': True}

    def invoke_build(self):
        with patch.object(commands, 'identity', return_value=self.current), \
             patch.object(commands, 'run') as run, \
             patch.object(commands, 'output', return_value='cache_miss\t2'), \
             patch.dict(os.environ, {'ESPYSPA_BUILD_JOBS': '4'}):
            metrics = {}
            commands.build(io.StringIO(), metrics)
            return [list(map(str, call.args[0])) for call in run.call_args_list], metrics

    def test_initial_and_unchanged_incremental(self):
        calls, metrics = self.invoke_build()
        idf = next(c for c in calls if c[0] == 'idf.py')
        self.assertIn('SDKCONFIG=' + str(commands.CONFIG), idf)
        self.assertEqual(idf[-1], 'build')
        self.assertEqual(metrics['ccache'], {'cache_miss': '2'})
        commands.BUILD.mkdir()
        (commands.BUILD / 'CMakeCache.txt').touch()
        calls, _ = self.invoke_build()
        self.assertEqual(sum(c[0] == 'idf.py' for c in calls), 1)
        self.assertNotIn('-D', next(c for c in calls if c[0] == 'idf.py'))
        self.assertFalse(any('fullclean' in c or 'set-target' in c for c in calls))

    def test_revision_change_reconfigures(self):
        commands.BUILD.mkdir()
        (commands.BUILD / 'CMakeCache.txt').touch()
        (self.state / 'identity.json').write_text('{}')
        calls, _ = self.invoke_build()
        self.assertEqual([c[-1] for c in calls if c[0] == 'idf.py'], ['reconfigure', 'build'])

    def test_development_identity_and_dirty_state(self):
        for dirty in ('', ' M main/firmware_web.cpp', '?? new-source.cpp'):
            with patch.object(commands, 'output', side_effect=['a' * 40, dirty]):
                result = commands.identity()
            self.assertEqual(result['channel'], 'development')
            self.assertEqual(result['dirty'], bool(dirty))
            self.assertEqual(result['version'].endswith('-dirty'), bool(dirty))
            self.assertIn('aaaaaaa', result['version'])
            self.assertLessEqual(len(result['version'].encode()), 31)

    def test_package_rewrites_only_staged_copy(self):
        commands.BUILD.mkdir()
        metadata = {'flash_files': {'0x20000': 'espyspa_matter.bin'},
                    'app': {'offset': '0x20000', 'file': 'espyspa_matter.bin'}}
        original = json.dumps(metadata)
        (commands.BUILD / 'flasher_args.json').write_text(original)
        for name in ('espyspa_matter.bin', 'quality-report.json', 'espyspa_matter.elf'):
            (commands.BUILD / name).write_bytes(b'local test image')
        package_spec = importlib.util.spec_from_file_location('packager', ROOT / 'scripts/package_firmware.py')
        packager = importlib.util.module_from_spec(package_spec)
        package_spec.loader.exec_module(packager)

        def run(args, log):
            if 'scripts/package_firmware.py' in args:
                packager.package_firmware(args[-1])

        with patch.object(commands, 'tests'), patch.object(commands, 'build'), \
             patch.object(commands, 'run', side_effect=run):
            commands.package(io.StringIO(), {'identity': self.current})
        self.assertEqual((commands.BUILD / 'flasher_args.json').read_text(), original)
        staged = json.loads((self.state / 'package/flasher_args.json').read_text())
        self.assertEqual(staged['app']['file'], 'espyspa-matter.bin')
        self.assertFalse((self.state / 'package/espyspa_matter.bin').exists())
        self.assertTrue((commands.BUILD / 'espyspa_matter.bin').exists())

    def test_bad_jobs_rejected_before_docker(self):
        result = subprocess.run([str(ROOT / 'scripts/local-build'), 'build'],
                                env={**os.environ, 'ESPYSPA_BUILD_JOBS': '0'},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('positive integer', result.stderr)

    def test_sdk_pins_match_ci(self):
        workflow = (ROOT / '.github/workflows/validate.yml').read_text()
        dockerfile = (ROOT / 'tools/docker/Dockerfile').read_text()
        image = next(line.split('container:', 1)[1].strip() for line in workflow.splitlines()
                     if line.strip().startswith('container:'))
        self.assertIn('FROM ' + image + '@sha256:', dockerfile)
        self.assertIn(commands.MATTER_REVISION, workflow)
        self.assertIn(commands.MATTER_REVISION, dockerfile)
        self.assertIn(commands.IDF_REVISION, dockerfile)

    def test_job_limit_reaches_nested_ninja(self):
        result = subprocess.run([str(ROOT / 'tools/docker/bin/ninja'), '-C', 'chip', 'esp32'],
                                env={**os.environ, 'ESPYSPA_NINJA': '/bin/echo',
                                     'ESPYSPA_BUILD_JOBS': '4'}, capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), '-j 4 -C chip esp32')


if __name__ == '__main__':
    unittest.main()
