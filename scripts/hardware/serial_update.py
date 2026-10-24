"""Explicit USB application update; no serial access during dry-run.

Transport is esptool 4.8.1. Only the inactive app and one IDF 5.4.1 OTA
selection sector may be written. This module is also the remote SSH worker.
"""
import argparse
import ast
import base64
import binascii
import contextlib
import fcntl
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse

SECTOR = 4096
FLASH_SIZE = 8 * 1024 * 1024
MAX_INPUT = 12 * 1024 * 1024
SERVICE = 'espyspa-serial.service'
ESPTOOL_VERSION = '4.8.1'
NEW, PENDING, VALID, INVALID, ABORTED = range(5)
EXPECTED = {
    'esp_secure_cert': (0x3f, 6, 0xd000, 0x2000, 1),
    'nvs': (1, 2, 0x10000, 0xc000, 0),
    'nvs_keys': (1, 4, 0x1c000, 0x1000, 1),
    'otadata': (1, 0, 0x1d000, 0x2000, 0),
    'phy_init': (1, 1, 0x1f000, 0x1000, 0),
    'ota_0': (0, 0x10, 0x20000, 0x3e0000, 0),
    'ota_1': (0, 0x11, 0x400000, 0x3e0000, 0),
    'fctry': (1, 2, 0x7e0000, 0x6000, 0),
}


class UpdateError(Exception):
    pass


class BoundedLog:
    encoding = 'utf-8'

    def __init__(self, target):
        self.target, self.size = target, 0

    def write(self, text):
        self.size += len(text.encode('utf-8'))
        require(self.size <= 1024 * 1024, 'SDK output exceeded private log limit')
        return self.target.write(text)

    def flush(self):
        self.target.flush()

    def isatty(self):
        return False


def require(condition, message):
    if not condition:
        raise UpdateError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def number(value):
    require(type(value) in (int, str), 'Invalid numeric metadata')
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError:
        raise UpdateError('Invalid numeric metadata') from None


def partition_table(data):
    require(len(data) == 3072, 'Expected a 3072-byte IDF partition table')
    entries = {}
    for offset in range(0, len(data), 32):
        record = data[offset:offset + 32]
        if record[:2] == b'\xeb\xeb':
            require(record[16:] == hashlib.md5(data[:offset]).digest(), 'Partition table MD5 mismatch')
            require(data[offset + 32:] == b'\xff' * (len(data) - offset - 32), 'Unexpected partition table suffix')
            require(entries == EXPECTED, 'Partition layout differs from the supported 8 MB baseline')
            return entries
        require(record[:2] == b'\xaa\x50', 'Partition table entry or MD5 record is missing')
        _, kind, subtype, start, size, label, flags = struct.unpack('<HBBII16sI', record)
        try:
            name = label.split(b'\0', 1)[0].decode('ascii')
        except UnicodeError:
            raise UpdateError('Invalid partition label') from None
        require(name not in entries, 'Duplicate partition label')
        entries[name] = (kind, subtype, start, size, flags)
    raise UpdateError('Partition table has no MD5 record')


def app_description(data):
    require(len(data) >= 288 and data[0] == 0xe9 and struct.unpack_from('<H', data, 12)[0] == 13,
            'Not an ESP32-C6 application image')
    require(struct.unpack_from('<I', data, 32)[0] == 0xabcd5432, 'Missing application descriptor')
    def text(offset):
        try:
            value = data[offset:offset + 32].split(b'\0', 1)[0].decode('ascii')
        except UnicodeError:
            raise UpdateError('Invalid application identity') from None
        require(bool(re.fullmatch(r'[A-Za-z0-9_.+\-]+', value)), 'Invalid application identity')
        return value
    require(text(80) == 'espyspa_matter', 'Application is not EspySpa-Matter')
    return text(48)


def validate_image(data):
    version = app_description(data)
    require(1 <= data[1] <= 16 and data[23] == 1, 'Unsupported segment count or missing image SHA-256')
    # IDF's app header can retain 40 MHz even with an 80 MHz bootloader.
    # We never rewrite either header: actual flash settings remain in bootloader.
    require(data[2] == 2 and data[3] in (0x30, 0x3f), 'Image must use the DIO, 8 MB baseline')
    offset, checksum = 24, 0xef
    for _ in range(data[1]):
        require(offset + 8 <= len(data), 'Truncated segment header')
        _, length = struct.unpack_from('<II', data, offset)
        offset += 8
        require(length % 4 == 0 and offset + length <= len(data), 'Truncated or unaligned image segment')
        for byte in data[offset:offset + length]:
            checksum ^= byte
        offset += length
    checksum_offset = ((offset + 16) // 16) * 16 - 1
    require(checksum_offset + 33 == len(data), 'Signed, merged, padded or truncated images are not supported')
    require(data[checksum_offset] == checksum, 'Application checksum mismatch')
    require(hashlib.sha256(data[:checksum_offset + 1]).digest() == data[checksum_offset + 1:],
            'Application embedded SHA-256 mismatch')
    return version


def load_package(directory):
    root = Path(directory).resolve()
    require(root.is_dir(), 'Supply an extracted artifact directory, not a standalone .bin')
    files = {}
    def read(name, limit):
        require(isinstance(name, str) and not Path(name).is_absolute(), 'Unsafe artifact path')
        path = (root / name).resolve()
        require(path.is_relative_to(root) and path.is_file(), 'Missing or unsafe artifact file')
        require(0 < path.stat().st_size <= limit, 'Artifact is empty or exceeds size bound')
        data = path.read_bytes()
        require(len(data) <= limit, 'Artifact exceeds size bound')
        files[name] = data
        return data
    for name in ('flasher_args.json', 'quality-report.json', 'manifest.json'):
        read(name, 65536)
    metadata = json.loads(files['flasher_args.json'])
    flash_files = metadata.get('flash_files', {})
    require(isinstance(flash_files, dict) and len(flash_files) == 4, 'Expected four baseline flashing images')
    for name in flash_files.values():
        read(name, FLASH_SIZE)
    require(sum(map(len, files.values())) < MAX_INPUT * 3 // 4, 'Artifact package exceeds transfer bound')
    validate_package(files)
    return files


def validate_package(files):
    metadata, quality, manifest = (json.loads(files[n]) for n in ('flasher_args.json', 'quality-report.json', 'manifest.json'))
    require(metadata.get('extra_esptool_args', {}).get('chip') == 'esp32c6', 'Metadata target is not ESP32-C6')
    settings = metadata.get('flash_settings')
    require(settings == {'flash_mode': 'dio', 'flash_size': '8MB', 'flash_freq': '80m'}, 'Unsupported flash parameters')
    offsets = {number(k): v for k, v in metadata['flash_files'].items()}
    require(set(offsets) == {0, 0xc000, 0x1d000, 0x20000}, 'Unexpected artifact flash offsets')
    require(len(offsets) == len(metadata['flash_files']) == 4, 'Duplicate artifact offsets')
    for key, offset in (('bootloader', 0), ('partition-table', 0xc000), ('otadata', 0x1d000), ('app', 0x20000)):
        section = metadata.get(key, {})
        require(number(section.get('offset')) == offset and section.get('file') == offsets[offset], 'Inconsistent flash metadata')
        require(section.get('encrypted') in (False, 'false'), 'Encrypted artifacts are not supported')
    checked = quality.get('configuration_checked', {})
    for key, value in {'CONFIG_IDF_TARGET': '"esp32c6"', 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB': 'y',
                       'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y', 'CONFIG_PARTITION_TABLE_FILENAME': '"partitions.csv"'}.items():
        require(checked.get(key) == value, 'Missing matching baseline configuration quality evidence')
    reports = quality.get('images', [])
    require(isinstance(reports, list) and len(reports) == 4, 'Incomplete image quality evidence')
    by_offset = {number(item['offset']): item for item in reports}
    require(set(by_offset) == set(offsets), 'Quality report offsets differ')
    for offset, name in offsets.items():
        data = files[name]
        report = by_offset[offset]
        require(len(data) == report.get('bytes') and digest(data) == report.get('sha256'), 'Artifact quality hash/size mismatch')
    table = files[offsets[0xc000]]
    partition_table(table)
    app = files[offsets[0x20000]]
    version = validate_image(app)
    require(len(app) <= EXPECTED['ota_0'][3] - 65536, 'Application does not leave required 64 KiB slot headroom')
    require(manifest.get('target') == 'esp32c6' and manifest.get('version') == version, 'Manifest target/version mismatch')
    require(manifest.get('channel') in ('release', 'development'), 'Invalid image channel')
    require(manifest.get('sha256') == digest(app) and manifest.get('bytes') == len(app), 'Manifest hash/size mismatch')
    require(manifest.get('commit') and str(quality.get('commit', '')).startswith(str(manifest['commit'])), 'Manifest/quality revisions differ')
    return {'version': version, 'image_channel': manifest['channel'], 'sha256': digest(app),
            'app': app, 'table': table, 'settings': settings}


def ota_plan(data):
    require(len(data) == 2 * SECTOR, 'Invalid OTA selection partition size')
    entries = []
    for index in range(2):
        sequence, state, crc = struct.unpack_from('<I20xII', data, index * SECTOR)
        valid_crc = crc == binascii.crc32(struct.pack('<I', sequence), 0xffffffff) & 0xffffffff
        if sequence not in (0, 0xffffffff) and valid_crc:
            require(state in (NEW, PENDING, VALID, INVALID, ABORTED, 0xffffffff), 'Unknown OTA state')
            require(state not in (NEW, PENDING), 'An OTA image is already new or pending verification')
            entries.append((sequence, state, index))
    bootable = [entry for entry in entries if entry[1] not in (INVALID, ABORTED)]
    require(bool(bootable), 'No confirmed boot-selection record; routine update refused')
    require(len(bootable) != 2 or bootable[0][0] != bootable[1][0], 'Ambiguous equal OTA sequences')
    sequence, state, active_record = max(bootable)
    require(state == VALID, 'Current image is not confirmed VALID; routine update refused')
    active_slot = (sequence - 1) % 2
    target_slot = 1 - active_slot
    next_sequence = max(e[0] for e in entries) + 1
    if (next_sequence - 1) % 2 != target_slot:
        next_sequence += 1
    require(next_sequence < 0xffffffff, 'OTA sequence exhausted; explicit recovery is required')
    sector = bytearray(b'\xff' * SECTOR)
    crc = binascii.crc32(struct.pack('<I', next_sequence), 0xffffffff) & 0xffffffff
    struct.pack_into('<I20xII', sector, 0, next_sequence, NEW, crc)
    return {'active_slot': active_slot, 'target_slot': target_slot, 'active_record': active_record,
            'new_record': 1 - active_record, 'sequence': next_sequence, 'sector': bytes(sector)}


class Host:
    def __init__(self, options):
        self.options = options

    def run(self, argv, timeout=10, allowed=(0,)):
        try:
            result = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout, check=False)
        except (OSError, subprocess.TimeoutExpired):
            raise UpdateError('Pi command unavailable or timed out') from None
        require(len(result.stdout) <= 1024 * 1024, 'Pi command output exceeds limit')
        require(result.returncode in allowed, 'Pi command failed; check required packages, permissions and service state')
        return result

    def service_state(self):
        state = self.run(['systemctl', 'show', SERVICE, '--property=ActiveState', '--value']).stdout.decode().strip()
        require(state in ('active', 'inactive'), 'Logging service is missing, failed or transitioning')
        return state == 'active'

    def service(self, action):
        self.run(['sudo', '-n', '/usr/bin/systemctl', action, SERVICE], timeout=15)
        require(self.service_state() == (action == 'start'), 'Logging service did not reach requested state')

    def device(self):
        configured = self.options.get('device')
        if configured is None:
            unit = self.run(['systemctl', 'cat', SERVICE]).stdout.decode()
            starts = [line[10:] for line in unit.splitlines() if line.startswith('ExecStart=') and line[10:]]
            require(len(starts) == 1, 'Cannot identify logger ExecStart; specify --device')
            sources = [Path(arg) for arg in shlex.split(starts[0]) if arg.startswith('/') and arg.endswith('.py')]
            require(len(sources) == 1 and sources[0].stat().st_size <= 65536, 'Cannot inspect logger; specify --device')
            tree = ast.parse(sources[0].read_text())
            devices = [node.value.value for node in tree.body if isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'DEVICE' for t in node.targets)
                       and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str)]
            require(len(devices) == 1, 'Logger has no constant DEVICE; specify --device')
            configured = devices[0]
        require(isinstance(configured, str) and bool(configured), 'Serial device override must not be empty')
        path = Path(configured)
        require(path.is_absolute() and str(path).startswith('/dev/'), 'Serial device must be an absolute /dev path')
        resolved = path.resolve(strict=True)
        require(stat.S_ISCHR(resolved.stat().st_mode), 'Serial device is not a character device')
        matches = [p for p in Path('/dev/serial/by-id').glob('*') if p.resolve() == resolved]
        require(bool(matches), 'No stable by-id identity for device; routine update refused')
        return str(path if path.parent == Path('/dev/serial/by-id') else sorted(matches)[0])

    def port_users(self, device):
        # Resolve identity to its current kernel node for narrowly scoped sudoers
        # rules; use the stable by-id identity for opening and re-enumeration.
        node = str(Path(device).resolve(strict=True))
        result = self.run(['sudo', '-n', '/usr/bin/fuser', '--', node], allowed=(0, 1))
        if result.returncode == 1:
            require(not result.stderr.strip(), 'Cannot establish all serial-port users; check fuser sudo permission')
            return set()
        require(bool(re.fullmatch(rb'[\d\s]+', result.stdout)), 'Unexpected serial-port ownership response')
        return {int(value) for value in result.stdout.split()}

    def logger_pid(self):
        text = self.run(['systemctl', 'show', SERVICE, '--property=MainPID', '--value']).stdout.decode().strip()
        require(text.isdigit(), 'Cannot read logger PID')
        return int(text)

    def wait_device(self, device, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if Path(device).exists() and stat.S_ISCHR(Path(device).stat().st_mode):
                return
            time.sleep(0.25)
        raise UpdateError('USB device did not re-enumerate under its stable identity')

    def log_mark(self):
        path = Path(self.options['log_path'])
        try:
            info = path.stat()
            return (info.st_ino, info.st_size)
        except OSError:
            return None

    def logs_since(self, mark):
        path = Path(self.options['log_path'])
        try:
            with path.open('rb') as source:
                info = os.fstat(source.fileno())
                offset = mark[1] if mark and info.st_ino == mark[0] and info.st_size >= mark[1] else 0
                source.seek(max(offset, info.st_size - 1024 * 1024))
                return source.read(1024 * 1024).decode(errors='replace')
        except OSError:
            return ''

    def http(self, endpoint):
        url = self.options.get('diagnostics_url')
        if not url:
            return None
        parsed = urllib.parse.urlsplit(url)
        require(parsed.scheme == 'http' and parsed.hostname and parsed.port in (None, 80)
                and parsed.path == '/api/diagnostics' and not parsed.username and not parsed.password
                and not parsed.query and not parsed.fragment, 'Invalid configured diagnostics URL')
        url = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, endpoint, '', ''))
        try:
            result = self.run(['curl', '-q', '--globoff', '--noproxy', '*', '--proto', '=http',
                               '--connect-timeout', '2', '--max-time', '4', '--max-filesize', '1048576', '-fsS', '--', url], timeout=6)
            data = json.loads(result.stdout)
            return data if isinstance(data, dict) else None
        except (UpdateError, ValueError):
            return None

    def observe(self, package, plan, before, mark):
        result = {'boot_verified': False, 'image_confirmed_valid': False, 'application_health': 'unknown',
                  'version_after': None, 'serial_startup_seen': False}
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            logs = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', self.logs_since(mark))
            versions = re.findall(r'App version:\s*([A-Za-z0-9_.+\-]+)', logs)
            result['serial_startup_seen'] = 'Starting EspySpa-Matter on ESP32-C6' in logs
            serial_version = package['version'] in versions
            firmware = self.http('/api/firmware')
            diagnostics = self.http('/api/diagnostics')
            if firmware:
                result['version_after'] = firmware.get('version')
                expected = firmware.get('version') == package['version'] and firmware.get('partition') == 'ota_' + str(plan['target_slot'])
                result['boot_verified'] = expected
                result['image_confirmed_valid'] = expected and firmware.get('ota_state') == VALID
                result['image_channel_matches'] = firmware.get('image_channel') == package['image_channel']
                if before and before.get('update_channel'):
                    result['update_channel_preserved'] = firmware.get('update_channel') == before['update_channel']
            elif serial_version and result['serial_startup_seen']:
                result['version_after'] = package['version']
                loaded = re.findall(r'Loaded app from partition at offset (0x[0-9a-fA-F]+)', logs)
                slot_matches = bool(loaded) and int(loaded[-1], 16) == EXPECTED['ota_' + str(plan['target_slot'])][2]
                changed_version = before and before.get('version') != package['version']
                result['boot_verified'] = bool(slot_matches or changed_version)
                result['image_confirmed_valid'] = result['boot_verified'] and 'OTA image marked valid' in logs
            if diagnostics and result['boot_verified']:
                thread = diagnostics.get('thread', {}).get('role') in (2, 3, 4)
                fabrics = diagnostics.get('matter', {}).get('fabric_count')
                result['fabric_count_after'] = fabrics
                if before and type(before.get('fabric_count')) is int:
                    result['fabric_count_preserved'] = fabrics == before['fabric_count']
                health = (thread and type(fabrics) is int and fabrics > 0 and result.get('fabric_count_preserved', True)
                          and result.get('update_channel_preserved', True) and result.get('image_channel_matches', True)
                          and (diagnostics.get('fresh') is True or not self.options['spa_connected']))
                result['application_health'] = 'healthy' if health else 'unhealthy'
            if result['image_confirmed_valid'] and (not self.options.get('diagnostics_url') or result['application_health'] == 'healthy'):
                break
            time.sleep(2)
        return result


class Flash:
    """Pinned adapter. Constructing it does not open a port; connect does."""
    def __init__(self, device, directory, manual=False):
        import esptool
        import esptool.loader
        require(esptool.__version__ == ESPTOOL_VERSION, 'Install exactly esptool 4.8.1 in the updater environment')
        self.tool, self.device, self.directory, self.manual = esptool, device, Path(directory), manual
        self.esp = None
        esptool.loader.WRITE_BLOCK_ATTEMPTS = 1

    def connect(self):
        from esptool.targets.esp32c6 import ESP32C6ROM
        import termios
        self.esp = ESP32C6ROM(self.device, 115200)
        fcntl.ioctl(self.esp._port.fileno(), termios.TIOCEXCL)
        self.esp.connect('no_reset' if self.manual else 'usb_reset', attempts=1)
        require(self.esp.get_chip_id() == 13 and not self.esp.secure_download_mode, 'Unexpected chip or secure download mode')
        require(not self.esp.get_secure_boot_enabled() and not self.esp.get_flash_encryption_enabled(), 'Secure boot/encrypted flash requires a separate update design')
        self.esp = self.esp.run_stub()
        self.esp.WRITE_FLASH_ATTEMPTS = 1
        # Uncompressed final blocks are padded to this size. Never let an OTA
        # metadata write spill past its single 4 KiB selection sector.
        self.esp.FLASH_WRITE_SIZE = SECTOR
        require((self.esp.flash_id() >> 16) & 0xff == 0x17, 'Device does not have the expected 8 MB flash')
        self.esp.flash_set_parameters(FLASH_SIZE)

    def read(self, address, size):
        return self.esp.read_flash(address, size)

    def write(self, address, data):
        require((address in (EXPECTED['ota_0'][2], EXPECTED['ota_1'][2]) and len(data) <= EXPECTED['ota_0'][3] - 65536)
                or (address in (0x1d000, 0x1e000) and len(data) == SECTOR), 'Write range is not allowed')
        path = self.directory / 'write.bin'
        path.write_bytes(data)
        with path.open('rb') as image:
            args = argparse.Namespace(chip='esp32c6', addr_filename=[(address, image)],
                flash_mode='keep', flash_freq='keep', flash_size='8MB', compress=False,
                no_compress=True, no_stub=False, force=False, encrypt=False, encrypt_files=None,
                ignore_flash_encryption_efuse_setting=False, erase_all=False, verify=False, diff=False)
            # Direct supported Python operations reuse one connected stub. CLI
            # main() would attempt another stub upload/reset between commands.
            self.tool.write_flash(self.esp, args)
            image.seek(0)
            self.tool.verify_flash(self.esp, args)

    def reset(self):
        self.esp.hard_reset()

    def close(self):
        if self.esp is not None:
            self.esp._port.close()


def perform(package, host, flash_factory=Flash, dry_run=False):
    result = {'dry_run': dry_run, 'version_before': None, 'version_after': None,
              'target_version': package['version'], 'target_image_channel': package['image_channel'],
              'flash_contents_verified': False, 'boot_selection_verified': False,
              'boot_verified': False, 'image_confirmed_valid': False, 'application_health': 'unknown',
              'service_restored': False, 'stage': 'preflight'}
    device = host.device()
    was_running = host.service_state()
    result['logger_was_running'] = was_running
    result['stable_device_detected'] = True
    before = host.http('/api/firmware')
    if before:
        result['version_before'] = before.get('version')
        require(not before.get('busy'), 'Firmware management reports an update in progress')
    diagnostics_before = host.http('/api/diagnostics')
    observation_before = dict(before or {})
    if diagnostics_before and type(diagnostics_before.get('matter', {}).get('fabric_count')) is int:
        result['fabric_count_before'] = diagnostics_before['matter']['fabric_count']
        observation_before['fabric_count'] = result['fabric_count_before']
    if dry_run:
        result.update(stage='dry_run_complete', service_restored=True,
                      deferred=['service/port sudo permissions', 'port ownership/exclusivity', 'USB download/reset capability', 'chip security/revision/flash size',
                                'on-device partition/OTA records', 'inactive slot choice', 'flash verification', 'boot and health'])
        return result
    # Dependency failures must occur before stopping the logger or opening serial.
    try:
        installed = importlib.metadata.version('esptool')
    except importlib.metadata.PackageNotFoundError:
        installed = None
    require(installed == ESPTOOL_VERSION, 'Install exactly esptool 4.8.1 in the updater Python environment')
    users = host.port_users(device)
    allowed = {host.logger_pid()} if was_running else set()
    require(not users - allowed, 'Another process owns the serial port; no processes were killed')
    mark = host.log_mark()
    restore_needed = False
    flash = None
    with tempfile.TemporaryDirectory(prefix='espyspa-serial-') as temporary:
        try:
            if was_running:
                restore_needed = True  # A timed-out stop may already have stopped it.
                host.service('stop')
            result['stage'] = 'exclusive_access'
            require(not host.port_users(device), 'Serial port remains in use; no processes were killed')
            flash = flash_factory(device, temporary, host.options.get('manual_download', False))
            result['stage'] = 'download_mode'
            flash.connect()
            actual_table = flash.read(0xc000, 3072)
            partition_table(actual_table)
            require(actual_table == package['table'], 'Device/artifact partition tables differ')
            original = flash.read(EXPECTED['otadata'][2], 2 * SECTOR)
            plan = ota_plan(original)
            result.update(active_partition='ota_' + str(plan['active_slot']), target_partition='ota_' + str(plan['target_slot']))
            active_offset = EXPECTED[result['active_partition']][2]
            result['version_before'] = app_description(flash.read(active_offset, 288))
            observation_before['version'] = result['version_before']
            if before:
                require(before.get('partition') == result['active_partition'] and before.get('version') == result['version_before']
                        and before.get('ota_state') == VALID, 'HTTP and flash boot-state evidence disagree')
            result['stage'] = 'application_write'
            flash.write(EXPECTED[result['target_partition']][2], package['app'])
            result['flash_contents_verified'] = True
            # Verify selection did not change during preparation before committing.
            require(flash.read(EXPECTED['otadata'][2], 2 * SECTOR) == original, 'OTA records changed before commit')
            result['stage'] = 'boot_selection_write'
            address = EXPECTED['otadata'][2] + plan['new_record'] * SECTOR
            flash.write(address, plan['sector'])
            updated = flash.read(EXPECTED['otadata'][2], 2 * SECTOR)
            expected = bytearray(original)
            expected[plan['new_record'] * SECTOR:(plan['new_record'] + 1) * SECTOR] = plan['sector']
            require(updated == expected, 'Boot selection verification failed; state uncertain; do not retry')
            result['boot_selection_verified'] = True
            result['stage'] = 'normal_boot'
            flash.reset()
            flash.close()
            flash = None
            host.wait_device(device)
        except (Exception, KeyboardInterrupt) as error:
            result['error'] = 'Update failed during ' + result['stage'] + '; preserve evidence and establish device state before any retry'
            if isinstance(error, UpdateError):
                result['reason'] = str(error)
        finally:
            if flash is not None:
                try:
                    flash.close()
                except Exception:
                    result['port_close_failed'] = True
            if restore_needed:
                try:
                    host.service('start')
                    result['service_restored'] = True
                except Exception:
                    result['service_restoration_error'] = 'Restore espyspa-serial.service manually; automatic restoration failed'
            else:
                result['service_restored'] = not was_running
    if 'error' not in result and result['service_restored']:
        result['stage'] = 'application_observation'
        try:
            result.update(host.observe(package, plan, observation_before, mark))
            result['stage'] = 'complete'
        except Exception:
            result['error'] = 'Flash verified, but application observation failed'
    return result


def entry(files, options, dry_run):
    """All raw SDK/serial logs stay private and bounded; never emit traceback."""
    os.umask(0o077)
    try:
        package = validate_package(files)
        if dry_run:
            return perform(package, Host(options), dry_run=True)
        state = Path.home() / '.local/state/espyspa-serial-update'
        state.mkdir(parents=True, exist_ok=True, mode=0o700)
        require(state.is_dir() and not state.is_symlink() and state.stat().st_uid == os.getuid()
                and not state.stat().st_mode & 0o077, 'Updater state must be private and owned by the current user')
        fd = os.open(state / 'update.lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as lock, tempfile.TemporaryFile(dir=state, mode='w+') as private_log:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # Bound upstream progress output and prevent credential-bearing core dumps.
            import resource
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            def interrupted(signum, frame):
                raise UpdateError('Updater interrupted')
            for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGALRM):
                signal.signal(sig, interrupted)
            signal.alarm(600)
            try:
                bounded = BoundedLog(private_log)
                with contextlib.redirect_stdout(bounded), contextlib.redirect_stderr(bounded):
                    result = perform(package, Host(options))
            finally:
                signal.alarm(0)
            fd = os.open(state / 'last-result.json', os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as output:
                output.write(json.dumps(result, sort_keys=True) + '\n')
            return result
    except UpdateError as error:
        return {'error': str(error), 'dry_run': dry_run, 'flash_contents_verified': False, 'boot_verified': False}
    except (Exception, KeyboardInterrupt):
        return {'error': 'Serial updater preflight failed; check artifact, configuration, dependencies, permissions and exclusive updater lock',
                'dry_run': dry_run, 'flash_contents_verified': False, 'boot_verified': False}


def remote_entry(payload):
    try:
        files = {name: base64.b64decode(data, validate=True) for name, data in payload['files'].items()}
        require(sum(map(len, files.values())) <= MAX_INPUT, 'Remote artifact exceeds bound')
        print(json.dumps(entry(files, payload['options'], payload['dry_run']), sort_keys=True))
    except Exception:
        print(json.dumps({'error': 'Invalid remote update payload'}))


def main(argv=None):
    import spa_toolkit as toolkit
    parser = argparse.ArgumentParser(description='Explicit inactive-slot ESP32-C6 USB update through the Pi')
    parser.add_argument('firmware', type=Path, help='extracted artifact directory with matching quality report')
    parser.add_argument('--config', type=Path, default=toolkit.ROOT / '.hardware.local.json')
    parser.add_argument('--dry-run', action='store_true', help='no serial access, resets or service changes')
    parser.add_argument('--local', action='store_true', help='run directly on the Pi instead of SSH')
    parser.add_argument('--device', help='explicit Pi serial device; resolved to a stable by-id path')
    parser.add_argument('--manual-download', action='store_true', help='operator already entered ROM download mode; no entry reset')
    parser.add_argument('--remote-python', help='absolute Pi Python executable containing esptool 4.8.1')
    parser.add_argument('--json', action='store_true', help='JSON output (default)')
    args = parser.parse_args(argv)
    try:
        files = load_package(args.firmware)
        defaults = toolkit.hardware_env()
        if args.local:
            cfg = json.loads(args.config.read_text()) if args.config.exists() else {}
        else:
            cfg = toolkit.config(args.config)
        options = {}
        for key in ('serial_device', 'diagnostics_url', 'log_path'):
            fallback = None if key == 'serial_device' else ''
            options[key] = os.environ.get('ESPYSPA_' + key.upper(), cfg.get(key, defaults.get('ESPYSPA_' + key.upper(), fallback)))
        options['device'] = args.device if args.device is not None else options['serial_device']
        options.pop('serial_device')
        options['log_path'] = options['log_path'] or '/var/log/espyspa/serial.log'
        require(Path(options['log_path']).is_absolute(), 'Log path must be absolute')
        connected = os.environ.get('ESPYSPA_SPA_CONNECTED', defaults.get('ESPYSPA_SPA_CONNECTED', 'true'))
        require(connected in ('true', 'false'), 'ESPYSPA_SPA_CONNECTED must be exactly true or false')
        options.update(spa_connected=connected == 'true', manual_download=args.manual_download)
        if args.local:
            result = entry(files, options, args.dry_run)
        else:
            if args.remote_python:
                require(args.remote_python.startswith('/') and '\n' not in args.remote_python, 'Remote Python path must be absolute')
                python = shlex.quote(args.remote_python)
            else:
                python = 'python3' if args.dry_run else '"$HOME/.local/share/espyspa-serial-update/venv/bin/python"'
            program = 'import json,sys; p=json.loads(sys.stdin.buffer.read(' + str(MAX_INPUT + 1) + ')); n={"__name__":"serial_update_worker"}; exec(p["source"],n); n["remote_entry"](p)'
            payload = dict(source=Path(__file__).read_text(), files={k: base64.b64encode(v).decode() for k, v in files.items()},
                           options=options, dry_run=args.dry_run)
            data = json.dumps(payload).encode()
            require(len(data) <= MAX_INPUT, 'Transfer exceeds bound')
            completed = subprocess.run(toolkit.ssh_argv(cfg, python + ' -c ' + shlex.quote(program)), input=data,
                                       capture_output=True, timeout=660, check=False)
            require(completed.returncode == 0 and len(completed.stdout) <= 1024 * 1024,
                    'SSH updater failed; inspect Pi last-result.json/service/device state before retrying')
            result = json.loads(completed.stdout)
        print(json.dumps(toolkit.redact(result), sort_keys=True))
        if args.dry_run:
            return 0 if result.get('stage') == 'dry_run_complete' else 2
        health_ok = result.get('application_health') == 'healthy' or (not options['diagnostics_url'] and result.get('application_health') == 'unknown')
        return 0 if result.get('image_confirmed_valid') and result.get('boot_verified') and result.get('service_restored') and 'error' not in result and health_ok else 1
    except (UpdateError, toolkit.DiagnosticError) as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 2
    except (Exception, KeyboardInterrupt):
        print(json.dumps({'error': 'Serial update rejected or transport failed; check inputs and Pi state before retrying'}), file=sys.stderr)
        return 2
