import binascii
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('serial_update', ROOT / 'scripts/hardware/serial_update.py')
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)


def image(version='1.2.3'):
    header = bytearray(24)
    header[:4] = bytes((0xe9, 1, 2, 0x30))
    struct.pack_into('<H', header, 12, 13)
    header[23] = 1
    desc = bytearray(256)
    struct.pack_into('<I', desc, 0, 0xabcd5432)
    desc[16:16 + len(version)] = version.encode()
    desc[48:62] = b'espyspa_matter'
    data = header + struct.pack('<II', 0x42800020, len(desc)) + desc
    checksum = 0xef
    for byte in desc:
        checksum ^= byte
    data.extend(b'\0' * (((len(data) + 16) // 16) * 16 - 1 - len(data)))
    data.append(checksum)
    return bytes(data) + hashlib.sha256(data).digest()


def table():
    data = b''
    for name, (kind, subtype, offset, size, flags) in u.EXPECTED.items():
        data += struct.pack('<HBBII16sI', 0x50aa, kind, subtype, offset, size, name.encode(), flags)
    data += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(data).digest()
    return data.ljust(3072, b'\xff')


def record(sequence, state=u.VALID):
    crc = binascii.crc32(struct.pack('<I', sequence), 0xffffffff) & 0xffffffff
    return struct.pack('<I20sII', sequence, b'\xff' * 20, state, crc).ljust(u.SECTOR, b'\xff')


def files():
    data = {'app.bin': image(), 'partitions.bin': table(), 'bootloader.bin': b'boot', 'initial.bin': b'\xff' * 8192}
    mapping = {'0x0': 'bootloader.bin', '0xc000': 'partitions.bin', '0x1d000': 'initial.bin', '0x20000': 'app.bin'}
    metadata = {'extra_esptool_args': {'chip': 'esp32c6'},
                'flash_settings': {'flash_mode': 'dio', 'flash_size': '8MB', 'flash_freq': '80m'}, 'flash_files': mapping}
    for key, offset in (('app', '0x20000'), ('bootloader', '0x0'), ('partition-table', '0xc000'), ('otadata', '0x1d000')):
        metadata[key] = {'offset': offset, 'file': mapping[offset], 'encrypted': 'false'}
    quality = {'commit': 'abcdef1234567', 'configuration_checked': {
        'CONFIG_IDF_TARGET': '"esp32c6"', 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB': 'y',
        'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y', 'CONFIG_PARTITION_TABLE_FILENAME': '"partitions.csv"'},
        'images': [{'file': name, 'offset': offset, 'bytes': len(data[name]), 'sha256': u.digest(data[name])} for offset, name in mapping.items()]}
    manifest = {'version': '1.2.3', 'channel': 'release', 'target': 'esp32c6', 'commit': 'abcdef1',
                'bytes': len(data['app.bin']), 'sha256': u.digest(data['app.bin'])}
    data.update({name: json.dumps(value).encode() for name, value in (
        ('flasher_args.json', metadata), ('quality-report.json', quality), ('manifest.json', manifest))})
    return data


class FakeHost:
    def __init__(self, active=True, slot=1):
        self.options = {'manual_download': False, 'spa_connected': True}
        self.active, self.slot, self.events = active, slot, []
        self.other_users = set()
        self.fail_stop = self.fail_start = self.fail_wait = False

    def device(self):
        return '/dev/serial/by-id/test'

    def service_state(self):
        return self.active

    def service(self, action):
        self.events.append(action)
        self.active = action == 'start'
        if (action == 'stop' and self.fail_stop) or (action == 'start' and self.fail_start):
            raise RuntimeError('secret must never appear in JSON')

    def http(self, endpoint):
        return {'version': '0.9.0', 'partition': 'ota_' + str(self.slot), 'ota_state': u.VALID,
                'busy': False, 'update_channel': 'development'}

    def port_users(self, device):
        return self.other_users | ({42} if self.active else set())

    def logger_pid(self):
        return 42

    def log_mark(self):
        return (1, 100)

    def wait_device(self, device):
        self.events.append('reenumerate')
        if self.fail_wait:
            raise u.UpdateError('USB identity timed out')

    def observe(self, package, plan, before, mark):
        self.events.append('observe')
        return {'boot_verified': True, 'image_confirmed_valid': True, 'application_health': 'healthy',
                'version_after': package['version']}


class FakeFlash:
    def __init__(self, host, slot=1):
        self.host = host
        self.original = record(slot + 1) + b'\xff' * u.SECTOR
        self.otadata = self.original
        self.writes = []
        self.fail = None

    def connect(self):
        self.host.events.append('connect')
        if self.fail == 'connect':
            raise RuntimeError('secret in SDK error')

    def read(self, offset, size):
        if offset == 0xc000:
            return table() if self.fail != 'table' else b'\0' * 3072
        if offset == 0x1d000:
            return self.otadata
        return image('0.9.0')[:288]

    def write(self, offset, data):
        self.writes.append((offset, data))
        self.host.events.append('write')
        if self.fail == 'app' and offset >= 0x20000:
            raise RuntimeError('credential must not appear')
        if offset < 0x20000:
            if self.fail == 'metadata':
                raise RuntimeError('metadata commit interrupted')
            start = offset - 0x1d000
            self.otadata = self.otadata[:start] + data + self.otadata[start + len(data):]
            if self.fail == 'metadata_verify':
                self.otadata = b'\0' + self.otadata[1:]

    def reset(self):
        self.host.events.append('reset')

    def close(self):
        self.host.events.append('close')


class SerialUpdateTests(unittest.TestCase):
    def execute(self, host=None, failure=None, slot=1):
        host = host or FakeHost(slot=slot)
        flash = FakeFlash(host, slot)
        flash.fail = failure
        with patch.object(u.importlib.metadata, 'version', return_value='4.8.1'):
            result = u.perform(u.validate_package(files()), host, lambda *args: flash)
        return result, host, flash

    def test_package_and_image_checks(self):
        data = files()
        self.assertEqual(u.validate_package(data)['version'], '1.2.3')
        for change in ('chip', 'hash', 'version', 'channel', 'offset', 'rollback', 'encrypted', 'revision'):
            broken = copy.deepcopy(data)
            if change in ('hash', 'version', 'channel', 'revision'):
                name = 'manifest.json'; obj = json.loads(broken[name])
                obj[{'hash': 'sha256', 'version': 'version', 'channel': 'channel', 'revision': 'commit'}[change]] = 'invalid'
            else:
                name = 'quality-report.json' if change == 'rollback' else 'flasher_args.json'
                obj = json.loads(broken[name])
                if change == 'chip':obj['extra_esptool_args']['chip'] = 'esp32'
                if change == 'offset':obj['app']['offset'] = '0x400000'
                if change == 'encrypted':obj['app']['encrypted'] = 'true'
                if change == 'rollback':obj['configuration_checked']['CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE'] = 'n'
            broken[name] = json.dumps(obj).encode()
            with self.subTest(change=change), self.assertRaises(u.UpdateError):
                u.validate_package(broken)
        for index in (0, 1, 12, 23, 40, -1):
            broken = bytearray(image());broken[index] ^= 1
            with self.subTest(index=index), self.assertRaises(u.UpdateError):
                u.validate_image(bytes(broken))
        with self.assertRaises(u.UpdateError):u.validate_image(image() + b'\xff' * 4096)

    def test_missing_quality_bin_only_and_unsafe_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in files().items():(root / name).write_bytes(data)
            self.assertTrue(u.load_package(root))
            with self.assertRaises(u.UpdateError):u.load_package(root / 'app.bin')
            (root / 'quality-report.json').unlink()
            with self.assertRaises(u.UpdateError):u.load_package(root)
            (root / 'quality-report.json').write_bytes(files()['quality-report.json'])
            metadata = json.loads((root / 'flasher_args.json').read_text())
            metadata['flash_files']['0x20000'] = '../outside.bin'
            (root / 'flasher_args.json').write_text(json.dumps(metadata))
            with self.assertRaises(u.UpdateError):u.load_package(root)

    def test_partition_table_rejects_corruption_and_other_layout(self):
        self.assertEqual(u.partition_table(table()), u.EXPECTED)
        for index in (0, 8, 256 + 16):
            bad = bytearray(table());bad[index] ^= 1
            with self.assertRaises(u.UpdateError):u.partition_table(bytes(bad))
        with self.assertRaises(u.UpdateError):u.partition_table(table()[:-1])

    def test_ota_selection_both_slots_crc_and_preserved_record(self):
        for slot in (0, 1):
            original = record(slot + 1) + record(6, u.ABORTED)
            plan = u.ota_plan(original)
            self.assertEqual(plan['active_slot'], slot)
            self.assertEqual(plan['target_slot'], 1 - slot)
            self.assertEqual(plan['new_record'], 1)
            sequence, state, crc = struct.unpack_from('<I20xII', plan['sector'])
            self.assertGreater(sequence, 6)
            self.assertEqual((sequence - 1) % 2, 1 - slot)
            self.assertEqual(state, u.NEW)
            self.assertEqual(crc, binascii.crc32(struct.pack('<I', sequence), 0xffffffff) & 0xffffffff)
            self.assertEqual(len(plan['sector']), u.SECTOR)
        plan = u.ota_plan(record(2) + record(3))
        self.assertEqual(plan['active_record'], 1)
        self.assertEqual(plan['new_record'], 0)

    def test_unconfirmed_ambiguous_exhausted_or_invalid_ota_refused(self):
        cases = [b'\xff' * 8192, record(1, u.NEW) + record(2), record(1, u.PENDING) + record(2),
                 record(1) + record(1), record(1, 0xffffffff) + b'\xff' * 4096,
                 record(0xfffffffe) + record(1), record(2, u.INVALID) + record(1, u.ABORTED)]
        for data in cases:
            with self.subTest(data=data[:4].hex()), self.assertRaises(u.UpdateError):u.ota_plan(data)

    def test_dry_run_never_touches_port_dependencies_or_service(self):
        host = FakeHost()
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(host, 'port_users', side_effect=AssertionError('port access')))
            stack.enter_context(patch.object(host, 'service', side_effect=AssertionError('service interruption')))
            stack.enter_context(patch.object(u.importlib.metadata, 'version', side_effect=AssertionError('esptool dependency')))
            result = u.perform(u.validate_package(files()), host, lambda *args: self.fail('serial connection'), dry_run=True)
        self.assertEqual(result['stage'], 'dry_run_complete')
        self.assertIn('inactive slot choice', result['deferred'])
        self.assertEqual(host.events, [])

    def test_updates_only_inactive_app_and_one_selection_sector(self):
        for slot in (0, 1):
            result, host, flash = self.execute(slot=slot)
            self.assertEqual(result['stage'], 'complete')
            self.assertTrue(result['flash_contents_verified'])
            self.assertTrue(result['boot_selection_verified'])
            self.assertTrue(result['service_restored'])
            self.assertTrue(result['image_confirmed_valid'])
            self.assertEqual([offset for offset, _ in flash.writes], [u.EXPECTED['ota_' + str(1 - slot)][2], 0x1e000])
            self.assertEqual(flash.otadata[:4096], flash.original[:4096])
            self.assertLess(host.events.index('reset'), host.events.index('start'))
            self.assertLess(host.events.index('start'), host.events.index('observe'))

    def test_failed_flash_metadata_or_reset_restores_service_no_retry(self):
        for failure in ('connect', 'table', 'app', 'metadata', 'metadata_verify'):
            with self.subTest(failure=failure):
                result, host, flash = self.execute(failure=failure)
                self.assertIn('error', result)
                self.assertTrue(result['service_restored'])
                self.assertNotIn('secret', json.dumps(result))
                self.assertEqual(host.events.count('start'), 1)
                self.assertNotIn('reset', host.events)
                self.assertLessEqual(len(flash.writes), 2)
                if failure == 'app':self.assertEqual(len(flash.writes), 1)

    def test_logger_initially_stopped_is_not_started(self):
        result, host, flash = self.execute(host=FakeHost(active=False))
        self.assertTrue(result['service_restored'])
        self.assertNotIn('stop', host.events)
        self.assertNotIn('start', host.events)

    def test_stop_timeout_and_restore_failure_are_reported(self):
        host = FakeHost();host.fail_stop = True
        result, host, flash = self.execute(host=host)
        self.assertTrue(result['service_restored'])
        self.assertEqual(host.events, ['stop', 'start'])
        self.assertEqual(flash.writes, [])
        host = FakeHost();host.fail_start = True
        result, host, flash = self.execute(host=host)
        self.assertIn('service_restoration_error', result)
        self.assertFalse(result['service_restored'])

    def test_other_port_users_fail_before_service_interruption(self):
        host = FakeHost();host.other_users = {99}
        with self.assertRaisesRegex(u.UpdateError, 'Another process'):
            self.execute(host=host)
        self.assertEqual(host.events, [])

    def test_reenumeration_failure_keeps_verified_flash_distinct_from_boot(self):
        host = FakeHost();host.fail_wait = True
        result, host, flash = self.execute(host=host)
        self.assertTrue(result['flash_contents_verified'])
        self.assertTrue(result['boot_selection_verified'])
        self.assertFalse(result['boot_verified'])
        self.assertTrue(result['service_restored'])

    def test_wait_uses_same_stable_path_with_timeout(self):
        host = u.Host({})
        with patch.object(u.Path, 'exists', side_effect=[False, False, True]), patch.object(u.Path, 'stat', return_value=types.SimpleNamespace(st_mode=stat.S_IFCHR)), patch.object(u.time, 'sleep') as sleep:
            host.wait_device('/dev/serial/by-id/test')
            self.assertEqual(sleep.call_count, 2)
        with patch.object(u.time, 'monotonic', side_effect=[0, 31]):
            with self.assertRaisesRegex(u.UpdateError, 're-enumerate'):
                host.wait_device('/dev/serial/by-id/test')

    def test_logger_device_source_is_data_not_executed_and_override_is_stable(self):
        alias = Path('/dev/serial/by-id/test-native-usb')
        node = Path('/dev/ttyACM7')
        original_stat, original_resolve = u.Path.stat, u.Path.resolve
        def device_stat(path, *args, **kwargs):
            if str(path).startswith('/dev/'):
                return types.SimpleNamespace(st_mode=stat.S_IFCHR)
            return original_stat(path, *args, **kwargs)
        def resolve(path, *args, **kwargs):
            if str(path).startswith('/dev/'):
                return node
            return original_resolve(path, *args, **kwargs)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'logger.py'
            source.write_text('raise RuntimeError("must not execute")\nDEVICE = "' + str(alias) + '"\n')
            host = u.Host({'device': None})
            output = subprocess.CompletedProcess([], 0, ('ExecStart=/usr/bin/python3 -u ' + str(source)).encode(), b'')
            with patch.object(host, 'run', return_value=output), patch.object(u.Path, 'stat', device_stat), patch.object(u.Path, 'resolve', resolve), patch.object(u.Path, 'glob', return_value=[alias]):
                self.assertEqual(host.device(), str(alias))
                host.options['device'] = str(node)
                self.assertEqual(host.device(), str(alias))
                host.options['device'] = ''
                with self.assertRaisesRegex(u.UpdateError, 'empty'):host.device()

    def test_adapter_write_and_verify_reuse_connection_without_erase_or_reset(self):
        flash = u.Flash.__new__(u.Flash)
        events = []
        flash.esp = object()
        def write(esp, args):
            self.assertIs(esp, flash.esp)
            self.assertFalse(args.erase_all or args.force or args.encrypt)
            self.assertEqual(args.flash_size, '8MB')
            events.append(('write', args.addr_filename[0][0], args.addr_filename[0][1].read()))
        def verify(esp, args):
            events.append(('verify', args.addr_filename[0][0], args.addr_filename[0][1].read()))
        flash.tool = types.SimpleNamespace(write_flash=write, verify_flash=verify)
        with tempfile.TemporaryDirectory() as directory:
            flash.directory = Path(directory)
            flash.write(0x400000, image())
            flash.write(0x1e000, record(3, u.NEW))
            with self.assertRaises(u.UpdateError):flash.write(0x10000, b'nvs')
            with self.assertRaises(u.UpdateError):flash.write(0x1d000, b'\xff' * 8192)
        self.assertEqual([e[0] for e in events], ['write', 'verify', 'write', 'verify'])
        self.assertEqual(events[0][2], events[1][2])

    def test_private_log_bound_does_not_limit_application_file_size(self):
        log = u.BoundedLog(io.StringIO())
        log.write('progress')
        with self.assertRaises(u.UpdateError):log.write('x' * (1024 * 1024))

    def test_connect_pins_chip_security_retry_policy_and_sector_block_size(self):
        from unittest.mock import MagicMock
        tool = types.ModuleType('esptool');tool.__version__ = '4.8.1'
        loader = types.ModuleType('esptool.loader');tool.loader = loader
        target = types.ModuleType('esptool.targets.esp32c6')
        rom = MagicMock();stub = MagicMock()
        rom.get_chip_id.return_value = 13
        rom.secure_download_mode = False
        rom.get_secure_boot_enabled.return_value = False
        rom.get_flash_encryption_enabled.return_value = False
        rom.run_stub.return_value = stub
        stub.flash_id.return_value = 0x1740ef
        target.ESP32C6ROM = MagicMock(return_value=rom)
        modules = {'esptool': tool, 'esptool.loader': loader, 'esptool.targets.esp32c6': target}
        with patch.dict(sys.modules, modules), patch.object(u.fcntl, 'ioctl'):
            flash = u.Flash('/dev/serial/by-id/test', '/tmp')
            target.ESP32C6ROM.assert_not_called()
            flash.connect()
            rom.connect.assert_called_once_with('usb_reset', attempts=1)
            self.assertEqual(stub.WRITE_FLASH_ATTEMPTS, 1)
            self.assertEqual(stub.FLASH_WRITE_SIZE, 4096)
            self.assertEqual(loader.WRITE_BLOCK_ATTEMPTS, 1)
            rom.get_secure_boot_enabled.return_value = True
            with self.assertRaises(u.UpdateError):
                u.Flash('/dev/serial/by-id/test', '/tmp').connect()

    def test_application_observation_compares_health_channels_and_fabrics(self):
        host = u.Host({'diagnostics_url': 'http://host/api/diagnostics', 'spa_connected': True})
        firmware = {'version': '1.2.3', 'partition': 'ota_0', 'ota_state': 2,
                    'image_channel': 'release', 'update_channel': 'development'}
        diag = {'fresh': True, 'thread': {'role': 3}, 'matter': {'fabric_count': 3}}
        with patch.object(host, 'logs_since', return_value='Starting EspySpa-Matter on ESP32-C6'), patch.object(host, 'http', side_effect=[firmware, diag]):
            result = host.observe({'version': '1.2.3', 'image_channel': 'release'}, {'target_slot': 0},
                                  {'update_channel': 'development', 'fabric_count': 3}, (1, 0))
        self.assertTrue(result['boot_verified'] and result['image_confirmed_valid'])
        self.assertTrue(result['fabric_count_preserved'] and result['update_channel_preserved'])
        self.assertEqual(result['application_health'], 'healthy')

    def test_observation_drops_previous_health_when_http_fails_before_confirmation(self):
        startup = 'App version: 1.2.3\nStarting EspySpa-Matter on ESP32-C6\n'
        pending = {'version': '1.2.3', 'partition': 'ota_0', 'ota_state': u.PENDING,
                   'image_channel': 'release', 'update_channel': 'development'}
        for connected in (True, False):
            with self.subTest(spa_connected=connected):
                host = u.Host({'diagnostics_url': 'http://host/api/diagnostics', 'spa_connected': connected})
                diag = {'fresh': connected, 'thread': {'role': 3}, 'matter': {'fabric_count': 3}}
                with patch.object(host, 'logs_since', side_effect=[startup, startup + 'OTA image marked valid\n']), patch.object(host, 'http', side_effect=[pending, diag, None, None]) as http, patch.object(u.time, 'sleep') as sleep, patch.object(u.time, 'monotonic', side_effect=[0, 1, 2, 91]):
                    result = host.observe({'version': '1.2.3', 'image_channel': 'release'}, {'target_slot': 0},
                                          {'version': '0.9.0', 'update_channel': 'development', 'fabric_count': 3}, (1, 0))
                self.assertTrue(result['boot_verified'] and result['image_confirmed_valid'])
                self.assertEqual(result['application_health'], 'unknown')
                self.assertEqual(http.call_count, 4)
                self.assertEqual(sleep.call_count, 2)

    def test_observation_waits_for_current_http_health_after_recovery(self):
        startup = 'App version: 1.2.3\nStarting EspySpa-Matter on ESP32-C6\n'
        pending = {'version': '1.2.3', 'partition': 'ota_0', 'ota_state': u.PENDING,
                   'image_channel': 'release', 'update_channel': 'development'}
        valid = dict(pending, ota_state=u.VALID)
        diag = {'fresh': True, 'thread': {'role': 3}, 'matter': {'fabric_count': 3}}
        host = u.Host({'diagnostics_url': 'http://host/api/diagnostics', 'spa_connected': True})
        confirmed = startup + 'OTA image marked valid\n'
        with patch.object(host, 'logs_since', side_effect=[startup, confirmed, confirmed]), patch.object(host, 'http', side_effect=[pending, diag, None, None, valid, diag]) as http, patch.object(u.time, 'sleep') as sleep, patch.object(u.time, 'monotonic', side_effect=[0, 1, 2, 3]):
            result = host.observe({'version': '1.2.3', 'image_channel': 'release'}, {'target_slot': 0},
                                  {'version': '0.9.0', 'update_channel': 'development', 'fabric_count': 3}, (1, 0))
        self.assertTrue(result['boot_verified'] and result['image_confirmed_valid'])
        self.assertEqual(result['application_health'], 'healthy')
        self.assertEqual(http.call_count, 6)
        self.assertEqual(sleep.call_count, 2)

    def test_serial_only_confirmation_keeps_http_health_unknown(self):
        host = u.Host({'diagnostics_url': '', 'spa_connected': True})
        logs = 'App version: 1.2.3\nStarting EspySpa-Matter on ESP32-C6\nOTA image marked valid\n'
        with patch.object(host, 'logs_since', return_value=logs), patch.object(host, 'http', return_value=None), patch.object(u.time, 'sleep') as sleep, patch.object(u.time, 'monotonic', side_effect=[0, 1]):
            result = host.observe({'version': '1.2.3', 'image_channel': 'release'}, {'target_slot': 0},
                                  {'version': '0.9.0'}, (1, 0))
        self.assertTrue(result['boot_verified'] and result['image_confirmed_valid'])
        self.assertEqual(result['application_health'], 'unknown')
        sleep.assert_not_called()

    def test_esptool_python_operations_with_memory_only_fake_chip(self):
        try:
            import esptool
        except ImportError:
            self.skipTest('Optional pinned-esptool API integration; stdlib mock suite needs no esptool')
        if esptool.__version__ != '4.8.1':self.skipTest('API integration requires pinned esptool 4.8.1')

        class MemoryChip:
            CHIP_NAME = 'ESP32-C6'
            IMAGE_CHIP_ID = 13
            BOOTLOADER_FLASH_OFFSET = 0
            CHIP_DETECT_MAGIC_REG_ADDR = 0
            FLASH_WRITE_SIZE = FLASH_SECTOR_SIZE = 4096
            IS_STUB = True
            WRITE_FLASH_ATTEMPTS = 1
            secure_download_mode = False

            def __init__(self):
                self.memory = bytearray(b'\xff' * u.FLASH_SIZE)
                self.address = 0
                self.written_ranges = []

            def get_secure_boot_enabled(self):return False
            def get_flash_encryption_enabled(self):return False
            def get_encrypted_download_disabled(self):return False
            def flash_id(self):return 0x1740ef
            def get_chip_revision(self):return 1
            def get_major_chip_version(self):return 0
            def get_minor_chip_version(self):return 1
            def flash_begin(self, size, address, **kwargs):
                self.address = address
                return (size + self.FLASH_WRITE_SIZE - 1) // self.FLASH_WRITE_SIZE
            def flash_block(self, block, sequence):
                start = self.address + sequence * self.FLASH_WRITE_SIZE
                self.memory[start:start + len(block)] = block
                self.written_ranges.append((start, start + len(block)))
            def read_reg(self, *args, **kwargs):return 0
            def flash_finish(self, reboot):
                if reboot:raise AssertionError('Unexpected reset')
            def flash_md5sum(self, address, size):
                return hashlib.md5(self.memory[address:address + size]).hexdigest()

        flash = u.Flash.__new__(u.Flash)
        flash.tool, flash.esp = esptool, MemoryChip()
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            flash.directory = Path(directory)
            flash.write(0x400000, image())
            flash.write(0x1d000, record(3, u.NEW))
        self.assertEqual(flash.esp.memory[0x1d000:0x1e000], record(3, u.NEW))
        self.assertEqual(flash.esp.memory[0x1e000:0x20000], b'\xff' * 8192)
        self.assertEqual(flash.esp.written_ranges[-1], (0x1d000, 0x1e000))

    def test_remote_cli_uses_shared_ssh_config_and_streams_validated_artifacts(self):
        tool_spec = importlib.util.spec_from_file_location('spa_toolkit', ROOT / 'scripts/hardware/spa_toolkit.py')
        toolkit = importlib.util.module_from_spec(tool_spec);tool_spec.loader.exec_module(toolkit)
        cfg = dict(ssh_host='test.invalid', ssh_user='tester', ssh_key='/tmp/example-key',
                   diagnostics_url='http://test.invalid/api/diagnostics', log_path='/var/log/espyspa/serial.log')
        with tempfile.TemporaryDirectory() as directory:
            for name, data in files().items():(Path(directory) / name).write_bytes(data)
            completed = subprocess.CompletedProcess([], 0, b'{"stage":"dry_run_complete"}', b'never expose stderr')
            with patch.dict(sys.modules, {'spa_toolkit': toolkit}), patch.object(toolkit, 'config', return_value=cfg), patch.object(toolkit, 'hardware_env', return_value={}), patch.object(u.subprocess, 'run', return_value=completed) as run, patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(u.main([directory, '--dry-run']), 0)
            argv = run.call_args.args[0]
            self.assertIn('StrictHostKeyChecking=yes', argv)
            self.assertIn('python3 -c', argv[-1])
            payload = json.loads(run.call_args.kwargs['input'])
            self.assertTrue(payload['dry_run'])
            self.assertEqual(payload['options']['diagnostics_url'], cfg['diagnostics_url'])
            self.assertEqual(run.call_args.kwargs['timeout'], 660)

    def test_cli_requires_known_health_when_http_is_configured(self):
        tool_spec = importlib.util.spec_from_file_location('spa_toolkit', ROOT / 'scripts/hardware/spa_toolkit.py')
        toolkit = importlib.util.module_from_spec(tool_spec);tool_spec.loader.exec_module(toolkit)
        with tempfile.TemporaryDirectory() as directory:
            for name, data in files().items():(Path(directory) / name).write_bytes(data)
            for url, health, expected in (('http://test.invalid/api/diagnostics', 'unknown', 1),
                                         ('http://test.invalid/api/diagnostics', 'healthy', 0),
                                         ('', 'unknown', 0)):
                with self.subTest(url=url, health=health):
                    cfg = dict(ssh_host='test.invalid', ssh_user='tester', ssh_key='/tmp/example-key',
                               diagnostics_url=url, log_path='/var/log/espyspa/serial.log')
                    result = {'image_confirmed_valid': True, 'boot_verified': True,
                              'service_restored': True, 'application_health': health}
                    completed = subprocess.CompletedProcess([], 0, json.dumps(result).encode(), b'')
                    with patch.dict(sys.modules, {'spa_toolkit': toolkit}), patch.object(toolkit, 'config', return_value=cfg), patch.object(toolkit, 'hardware_env', return_value={}), patch.object(u.subprocess, 'run', return_value=completed), patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(u.main([directory]), expected)


if __name__ == '__main__':
    unittest.main()
