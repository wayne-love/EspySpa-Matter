"""Allowlisted Matter reads through the existing Pi SSH transport.

The remote worker returns parsed attribute values only, never chip-tool logs.
It is sent over SSH, so the Pi needs no copy of this repository.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import resource
import shlex
import stat
import subprocess
import tempfile
import time

REVISION = 'a6892983d6d761a9a787f2e11177a85d28bd1022'
LIMIT = 1024 * 1024
ATTRIBUTES = {
    0x1d: {0: 'DeviceTypeList', 1: 'ServerList', 3: 'PartsList'},
    0x28: {1: 'VendorName', 2: 'VendorID', 3: 'ProductName', 4: 'ProductID',
           7: 'HardwareVersion', 9: 'SoftwareVersion', 10: 'SoftwareVersionString',
           0x14: 'ProductURL'},
    0x402: {0: 'MeasuredValue', 1: 'MinMeasuredValue', 2: 'MaxMeasuredValue'},
    0x201: {0: 'LocalTemperature', 0x12: 'OccupiedHeatingSetpoint',
            0x1c: 'SystemMode', 0x29: 'ThermostatRunningState',
            0x15: 'MinHeatSetpointLimit', 0x16: 'MaxHeatSetpointLimit'},
    0x202: {0: 'FanMode', 1: 'FanModeSequence', 2: 'PercentSetting',
            3: 'PercentCurrent', 4: 'SpeedMax', 5: 'SpeedSetting', 6: 'SpeedCurrent'},
    0x6: {0: 'OnOff'},
    0x50: {0: 'Description', 3: 'CurrentMode'},
}


def integer(value, maximum):
    if isinstance(value, str) and re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', value):
        value = int(value, 16 if value.lower().startswith('0x') else 10)
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('Invalid numeric Matter configuration')
    return value


def request(node, endpoint, cluster, attribute):
    result = dict(node=integer(node, 0xffffffefffffffff),
                  endpoint=integer(endpoint, 65534), cluster=integer(cluster, 0xffffffff),
                  attribute=integer(attribute, 0xffffffff))
    if result['node'] == 0:
        raise ValueError('Matter node ID must be nonzero')
    cluster, attribute = result['cluster'], result['attribute']
    if cluster not in ATTRIBUTES or (attribute not in ATTRIBUTES[cluster] and attribute != 0xfffb):
        raise ValueError('Matter attribute is not in the read-only allowlist')
    return result


def scalar(text):
    if text == 'null':
        return None
    if text in ('TRUE', 'FALSE'):
        return text == 'TRUE'
    if re.fullmatch(r'-?[0-9]+', text):
        return int(text)
    return text


def parse_report(output, req, returncode):
    """Parse the pinned DataModelLogger format; reject missing/truncated data."""
    if len(output.encode()) > LIMIT:
        raise ValueError('Matter output exceeded 1 MiB')
    output = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', output)
    if returncode:
        # Never expose arbitrary error/log lines, even on a parse failure.
        if re.search(r'\b(?:UnsupportedAttribute|UNSUPPORTED_ATTRIBUTE|UnsupportedCluster|UNSUPPORTED_CLUSTER)\b', output):
            return {'status': 'unsupported', 'reason': 'Matter reports unsupported attribute or cluster'}
        return {'status': 'failed', 'reason': 'chip-tool failed', 'exit_code': returncode}
    lines = []
    for line in output.splitlines():
        match = re.search(r'(?:CHIP:TOO:|\[TOO\])\s*(.*)$', line)
        if match:
            lines.append(match[1].strip())
    mei = r'0x[0-9A-Fa-f]+(?:_[0-9A-Fa-f]+)*'
    header = re.compile(r'Endpoint: (\d+) Cluster: (' + mei + r') Attribute (' + mei + r') DataVersion: \d+$')
    start = None
    for i, line in enumerate(lines):
        match = header.fullmatch(line)
        if match and tuple(int(v, 0) for v in match.groups()) == (req['endpoint'], req['cluster'], req['attribute']):
            start = i + 1
            break
    if start is None:
        return {'status': 'failed', 'reason': 'No matching attribute report'}
    label = 'AttributeList' if req['attribute'] == 0xfffb else ATTRIBUTES[req['cluster']][req['attribute']]
    body = []
    for line in lines[start:]:
        if line.startswith('Endpoint:'):
            break
        body.append(line)
    if not body or not body[0].startswith(label + ': '):
        return {'status': 'failed', 'reason': 'Unrecognised attribute value'}
    text = body[0][len(label) + 2:]
    count = re.fullmatch(r'(\d+) entries', text)
    if not count and label in ('AttributeList', 'DeviceTypeList', 'ServerList', 'PartsList'):
        return {'status': 'failed', 'reason': 'Expected an attribute list'}
    if not count:
        value = scalar(text)
        strings = ('VendorName', 'ProductName', 'SoftwareVersionString', 'ProductURL', 'Description')
        valid = (isinstance(value, str) if label in strings else
                 type(value) is bool if label == 'OnOff' else
                 value is None or type(value) is int)
        if not valid:
            return {'status': 'failed', 'reason': 'Invalid scalar attribute value'}
        return {'status': 'supported', 'value': value}
    expected = int(count[1])
    if expected > 512 or any('truncated' in line.lower() for line in body):
        return {'status': 'failed', 'reason': 'Invalid or oversized attribute list'}
    values = []
    if label == 'DeviceTypeList':
        entries = [i for i, line in enumerate(body) if re.fullmatch(r'\[\d+\]: \{', line)]
        for number, index in enumerate(entries, 1):
            if body[index] != f'[{number}]: {{':
                return {'status': 'failed', 'reason': 'Invalid device type indices'}
            chunk = body[index + 1:index + 4]
            if len(chunk) != 3 or chunk[2] != '}':
                return {'status': 'failed', 'reason': 'Invalid device type structure'}
            device = re.fullmatch(r'DeviceType: (\d+)(?: \([^\n]*\))?', chunk[0])
            revision = re.fullmatch(r'Revision: (\d+)', chunk[1])
            if not device or not revision:
                return {'status': 'failed', 'reason': 'Invalid device type structure'}
            values.append({'device_type': int(device[1]), 'revision': int(revision[1])})
    else:
        for line in body[1:]:
            item = re.fullmatch(r'\[(\d+)\]: (\d+)(?: \([^\n]*\))?', line)
            if item:
                if int(item[1]) != len(values) + 1:
                    return {'status': 'failed', 'reason': 'Invalid attribute list indices'}
                values.append(int(item[2]))
    if len(values) != expected:
        return {'status': 'failed', 'reason': 'Incomplete attribute list'}
    return {'status': 'supported', 'value': values}


def private_path(path, directory=False):
    info = path.lstat()
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    if not kind(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError('Controller paths must be private and owned by the SSH user')


@contextlib.contextmanager
def controller(req):
    """One user, one private store, one process at a time."""
    state = Path.home() / '.local/state/espyspa-matter'
    storage = state / 'controller'
    metadata = state / 'commissioned.json'
    private_path(state, directory=True)
    private_path(storage, directory=True)
    private_path(metadata)
    if metadata.stat().st_size > 1024:
        raise ValueError('Invalid commissioned-node metadata')
    saved = json.loads(metadata.read_text())
    if saved.get('node_id') != req['node'] or saved.get('revision') != REVISION:
        raise ValueError('Node ID or controller revision differs from commissioning record')
    binary = Path.home() / '.local/lib/espyspa-matter/chip-tool'
    fd = os.open(state / 'controller.lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield state, storage, binary


def bounded_child():
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_FSIZE, (LIMIT + 1, LIMIT + 1))


def remote_read(req):
    """Pi-only worker. It never commissions or creates controller storage."""
    req = request(**req)
    with controller(req) as (state, storage, binary):
        argv = [str(binary), 'any', 'read-by-id', str(req['cluster']), str(req['attribute']),
                str(req['node']), str(req['endpoint']), '--storage-directory', str(storage),
                '--commissioner-name', 'alpha', '--timeout', '15']

        with tempfile.TemporaryFile(dir=state) as output:
            try:
                completed = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=output,
                                           stderr=subprocess.STDOUT, timeout=22,
                                           preexec_fn=bounded_child, check=False)
            except subprocess.TimeoutExpired:
                return {'status': 'failed', 'reason': 'Matter read exceeded 22 seconds'}
            output.seek(0)
            text = output.read(LIMIT + 1).decode('utf-8', errors='replace')
            return parse_report(text, req, completed.returncode)


def remote_subscribe(req):
    """A single private process; shutdown affects only its own subscriptions."""
    req = request(**req)
    with controller(req) as (state, storage, binary), tempfile.TemporaryFile(dir=state) as output:
        argv = [str(binary), 'interactive', 'start', '--storage-directory', str(storage),
                '--commissioner-name', 'alpha']
        command = ('any subscribe-by-id {cluster} {attribute} 2 5 {node} {endpoint} '
                   '--keepSubscriptions 1 --auto-resubscribe 0 --timeout 8\n').format(**req)
        process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=output,
                                   stderr=subprocess.STDOUT, preexec_fn=bounded_child)
        timed_out = False
        try:
            process.stdin.write(command.encode())
            process.stdin.flush()
            time.sleep(12)
            process.communicate(b'subscriptions shutdown-all\nquit\n', timeout=10)
        except (subprocess.TimeoutExpired, BrokenPipeError):
            timed_out = True
            process.kill()
            process.communicate()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        output.seek(0)
        text = output.read(LIMIT + 1).decode('utf-8', errors='replace')
        if timed_out:
            return {'status': 'failed', 'reason': 'Subscription process required forced termination',
                    'clean_termination': False}
        parsed = parse_report(text, req, process.returncode)
        established = 'Subscription established' in text
        if parsed['status'] == 'supported' and not established:
            parsed = {'status': 'failed', 'reason': 'No subscription establishment confirmation'}
        parsed.update(established=established, duration_seconds=12,
                      clean_termination=process.returncode == 0)
        return parsed


def snapshot(node, read):
    """Discover a bounded endpoint tree and read only advertised attributes."""
    result = {'node_id': node, 'supported': [], 'unsupported': [], 'failed': []}
    calls = 0

    def record(endpoint, cluster, attribute):
        nonlocal calls
        calls += 1
        if calls > 256:
            raise ValueError('Matter snapshot exceeded 256-read limit')
        req = request(node, endpoint, cluster, attribute)
        response = read(req)
        status = response.get('status')
        if status not in ('supported', 'unsupported', 'failed'):
            raise ValueError('Invalid Matter worker response')
        result[status].append(dict(endpoint=endpoint, cluster=cluster, attribute=attribute, **response))
        return response.get('value') if status == 'supported' else None

    pending, seen = [0], set()
    while pending:
        endpoint = pending.pop(0)
        if endpoint in seen:
            continue
        if len(seen) >= 16:
            raise ValueError('Matter topology exceeded 16-endpoint limit')
        seen.add(endpoint)
        parts = record(endpoint, 0x1d, 3)
        if isinstance(parts, list):
            pending.extend(integer(part, 65534) for part in parts if part not in seen)
        record(endpoint, 0x1d, 0)
        servers = record(endpoint, 0x1d, 1)
        if not isinstance(servers, list):
            continue
        for cluster, attributes in ATTRIBUTES.items():
            if cluster == 0x1d or cluster not in servers:
                continue
            advertised = record(endpoint, cluster, 0xfffb)
            if not isinstance(advertised, list):
                continue
            for attribute in attributes:
                if attribute in advertised:
                    record(endpoint, cluster, attribute)
                else:
                    result['unsupported'].append(dict(endpoint=endpoint, cluster=cluster,
                        attribute=attribute, status='unsupported', reason='Not advertised in AttributeList'))
    result['endpoint_ids'] = sorted(seen)
    result['complete'] = not result['failed']
    return result


def main(argv=None):
    import sys
    import spa_toolkit as toolkit
    parser = argparse.ArgumentParser(description='Read-only Matter endpoint and attribute discovery through the Pi')
    parser.add_argument('--config', type=Path, default=toolkit.ROOT / '.hardware.local.json')
    parser.add_argument('--json', action='store_true', help='emit JSON (default)')
    parser.add_argument('--subscribe', action='store_true', help='also test one attribute subscription for 12 seconds')
    args = parser.parse_args(argv)
    try:
        cfg = toolkit.config(args.config)
        defaults = toolkit.hardware_env()
        node = os.environ.get('ESPYSPA_MATTER_NODE_ID', cfg.get('matter_node_id', defaults.get('ESPYSPA_MATTER_NODE_ID')))
        if node is None:
            raise ValueError('Set ESPYSPA_MATTER_NODE_ID to the recorded commissioned Pi node ID')
        node = request(node, 0, 0x1d, 3)['node']
        source = Path(__file__).read_text()

        def read(req, worker='remote_read'):
            program = 'ns = {"__name__": "spa_matter_remote"}\nexec(' + repr(source) + ', ns)\n'
            program += ('try:\n print(ns["json"].dumps(ns[' + repr(worker) + '](' + repr(req) + ')))\n'
                        'except Exception:\n print(\'{"status":"failed","reason":"Pi Matter worker failed; check installation, private storage, node record and lock"}\')\n')
            try:
                response = json.loads(toolkit.ssh(cfg, 'python3 -c ' + shlex.quote(program)))
            except toolkit.DiagnosticError as error:
                return {'status': 'failed', 'reason': str(error)}
            return response

        result = snapshot(node, read)
        if args.subscribe:
            candidates = [entry for entry in result['supported']
                          if (entry['cluster'], entry['attribute']) in ((0x201, 0), (0x402, 0), (0x28, 9))]
            candidates.sort(key=lambda entry: entry['cluster'] == 0x28)
            if candidates:
                entry = candidates[0]
                req = request(node, entry['endpoint'], entry['cluster'], entry['attribute'])
                result['subscription'] = read(req, 'remote_subscribe')
            else:
                result['subscription'] = {'status': 'failed', 'reason': 'No discovered subscription candidate'}
            result['complete'] = result['complete'] and result['subscription']['status'] == 'supported'
        print(json.dumps(toolkit.redact(result), sort_keys=True))
        return 0 if result['complete'] else 1
    except (toolkit.DiagnosticError, ValueError, OSError) as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 2
