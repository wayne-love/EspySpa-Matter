"""Explicit Matter writes using the existing private Pi controller identity."""
import argparse
from decimal import Decimal, InvalidOperation
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import urllib.parse

import spa_matter as matter

PROPERTIES = {
    'setpoint': (0x201, 0x12), 'percent': (0x202, 2), 'speed': (0x202, 5),
    'fan-mode': (0x202, 0), 'on-off': (6, 0), 'mode': (0x50, 3),
}


class OperationTimeout(Exception):
    pass


def rf_polls(data):
    counters = (data or {}).get('counters')
    value = counters.get('polls_ok') if isinstance(counters, dict) else None
    return value if type(value) is int else None


def parse_value(prop, value):
    if prop not in PROPERTIES:
        raise ValueError('Property is not in the writable allowlist')
    if prop == 'on-off':
        if value not in ('on', 'off'):
            raise ValueError('on-off requires on or off; toggle is not supported')
        return value == 'on'
    if prop == 'setpoint':
        try:
            number = Decimal(value)
            if not number.is_finite() or not Decimal(5) <= number <= Decimal(41) or number % Decimal('0.2'):
                raise ValueError('Setpoint must be 5–41 °C in 0.2 °C steps')
            return int(number * 100)
        except (InvalidOperation, TypeError):
            raise ValueError('Invalid temperature setpoint') from None
    maximum = {'percent': 100, 'speed': 255, 'fan-mode': 6, 'mode': 255}[prop]
    return matter.integer(value, maximum)


def command(prop, value, node, endpoint):
    if prop == 'on-off':
        return ['onoff', 'on' if value else 'off', str(node), str(endpoint)]
    if prop == 'mode':
        return ['modeselect', 'change-to-mode', str(value), str(node), str(endpoint)]
    cluster, attribute = PROPERTIES[prop]
    encoded = json.dumps('s:' + str(value)) if prop == 'setpoint' else str(value)
    return ['any', 'write-by-id', str(cluster), str(attribute), encoded, str(node), str(endpoint)]


def diagnostics(url):
    if not url:
        return None
    parsed = urllib.parse.urlsplit(url)
    if not (parsed.scheme == 'http' and parsed.hostname and parsed.port in (None, 80)
            and parsed.path == '/api/diagnostics' and not parsed.username and not parsed.password
            and not parsed.query and not parsed.fragment):
        raise ValueError('Invalid configured diagnostics URL')
    try:
        result = subprocess.run(['curl', '-q', '--globoff', '--noproxy', '*', '--proto', '=http',
            '--connect-timeout', '2', '--max-time', '4', '--max-filesize', '1048576', '-fsS', '--', url],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=6, check=False)
        if result.returncode or len(result.stdout) > matter.LIMIT:
            return None
        data = json.loads(result.stdout)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def native_target(prop, value, blower, modes, maximum):
    """Exact mappings from the firmware control policy, not inferred heat logic."""
    if prop == 'mode':
        return {value}
    if prop == 'on-off':
        return ({1, 11, 12, 13, 14, 15} if value else {2}) if blower else ({m for m in modes if m in (1, 2, 3)} if value else {0})
    if prop == 'fan-mode' and value in (4, 6):
        value = 5 if (1 if blower else 4) in modes else 3
    if prop == 'fan-mode' and value == 5:
        return {1 if blower else 4}
    if prop == 'percent':
        speed = (value * maximum + 99) // 100
    elif prop == 'speed':
        speed = value
    elif value == 0:
        speed = 0
    else:
        if (value == 1 and maximum < 2) or (value == 2 and maximum < 3):
            raise ValueError('Fan mode is unsupported by this speed range')
        speed = 1 if value == 1 else (maximum + 1) // 2 if value == 2 else maximum
    if speed == 0:
        return {2 if blower else 0}
    if blower:
        return {10 + speed}
    manual = [mode for mode in (3, 1, 2) if mode in modes]
    if len(manual) != maximum or speed > len(manual):
        raise ValueError('Fan speed capability does not match advertised modes')
    return {manual[speed - 1]}


def operate(endpoint, prop, raw_value, dry_run=False, url='', node=None, read=None, get_http=diagnostics):
    value = parse_value(prop, raw_value)
    endpoint = matter.integer(endpoint, 65534)
    if endpoint == 0:
        raise ValueError('Root properties are not writable through this command')
    if node is None:
        metadata = Path.home() / '.local/state/espyspa-matter/commissioned.json'
        matter.private_path(metadata)
        if metadata.stat().st_size > 1024:
            raise ValueError('Commissioning record exceeds limit')
        node = json.loads(metadata.read_text())['node_id']
    cluster, attribute = PROPERTIES[prop]
    req = matter.request(node, endpoint, cluster, attribute)
    with matter.controller(req) as context:
        def get(ep, cl, attr):
            response = (read or matter.read_locked)(matter.request(node, ep, cl, attr), context)
            if response.get('status') != 'supported':
                raise ValueError('Required discovery/readback attribute is unavailable')
            return response['value']
        parts = get(0, 0x1d, 3)
        if not isinstance(parts, list) or endpoint not in parts:
            raise ValueError('Endpoint is not advertised in root PartsList')
        servers = get(endpoint, 0x1d, 1)
        if cluster not in servers:
            raise ValueError('Target cluster is not advertised on this endpoint')
        attrs = get(endpoint, cluster, 0xfffb)
        if attribute not in attrs:
            raise ValueError('Target attribute is not advertised')
        before_value = get(endpoint, cluster, attribute)
        before_rf = get_http(url)
        if before_rf is not None and before_rf.get('fresh') is not True:
            raise ValueError('Spa RF state is stale; write was not sent')
        modes, expected_modes, blower = [], None, False
        if cluster == 0x202 or prop == 'mode' or (prop == 'on-off' and 0x202 in servers):
            if 0x50 not in servers:
                raise ValueError('Fan has no native Mode Select readback')
            description = get(endpoint, 0x50, 0)
            if description not in ('Spa pump operating mode', 'Spa blower mode and Variable level'):
                raise ValueError('Unknown native control identity')
            blower = description == 'Spa blower mode and Variable level'
            modes = get(endpoint, 0x50, 2)
            maximum = get(endpoint, 0x202, 4)
            if type(maximum) is not int or not 1 <= maximum <= 5 or (prop == 'speed' and value > maximum):
                raise ValueError('Requested speed exceeds the advertised maximum')
            expected_modes = native_target(prop, value, blower, modes, maximum)
            if not expected_modes or not expected_modes.issubset(set(modes)):
                raise ValueError('Requested operating mode is not advertised')
        elif prop == 'on-off':
            device_types = get(endpoint, 0x1d, 0)
            if not any(isinstance(item, dict) and item.get('device_type') == 0x100 for item in device_types):
                raise ValueError('OnOff endpoint is not a recognized eSpa light or fan')
        result = {'node_id': node, 'endpoint': endpoint, 'property': prop, 'requested_value': raw_value,
                  'before': before_value, 'dry_run': dry_run, 'write_attempted': False,
                  'accepted': False, 'rf_confirmed': False}
        if dry_run:
            result['status'] = 'dry_run_complete'
            return result
        state, storage, binary = context
        argv = [str(binary)] + command(prop, value, node, endpoint) + [
            '--storage-directory', str(storage), '--commissioner-name', 'alpha', '--timeout', '15']
        result['write_attempted'] = True
        with tempfile.TemporaryFile(dir=state) as output:
            try:
                completed = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=output,
                    stderr=subprocess.STDOUT, timeout=22, preexec_fn=matter.bounded_child, check=False)
            except (subprocess.TimeoutExpired, OSError, OperationTimeout):
                return dict(result, status='uncertain', accepted=None,
                            reason='Write outcome unknown; inspect RF state before another command')
            if completed.returncode:
                return dict(result, status='uncertain', accepted=None, exit_code=completed.returncode,
                            reason='Controller did not confirm acceptance; inspect RF before another command')
        result['accepted'] = True
        result['status'] = 'accepted_unverified'
        previous_polls = rf_polls(before_rf)
        deadline = time.monotonic() + 12
        for _ in range(4):
            try:
                rf = get_http(url)
                result['readback'] = get(endpoint, cluster, attribute)
                result['readback_matches'] = result['readback'] == value
                polls = rf_polls(rf)
                fresh = rf and rf.get('fresh') is True and type(polls) is int and type(previous_polls) is int and polls > previous_polls
                if fresh:
                    native = rf.get('last_known_spa_state', {})
                    if prop == 'setpoint':
                        result['rf_confirmed'] = Decimal(str(native.get('setpoint_c'))) * 100 == value
                    elif expected_modes is None:
                        result['rf_confirmed'] = type(native.get('light')) is bool and native['light'] == value
                    else:
                        mode = get(endpoint, 0x50, 3)
                        result['native_mode_readback'] = mode
                        result['rf_confirmed'] = mode in expected_modes
                if result['rf_confirmed']:
                    result['status'] = 'confirmed'
                    break
            except Exception:
                result['reason'] = 'Accepted request; readback unavailable; do not retry automatically'
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(2)
        return result


def main(argv=None):
    import spa_toolkit as toolkit
    parser = argparse.ArgumentParser(description='Explicit allowlisted Matter writes through the existing Pi identity')
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('property', choices=PROPERTIES)
    parser.add_argument('value')
    parser.add_argument('--dry-run', action='store_true', help='discovery and reads only; never writes')
    parser.add_argument('--local', action='store_true', help='run directly on the Pi')
    parser.add_argument('--config', type=Path)
    parser.add_argument('--json', action='store_true', help='JSON output (default)')
    args = parser.parse_args(argv)
    try:
        parse_value(args.property, args.value)
        endpoint = matter.integer(args.endpoint, 65534)
        if args.local:
            path = args.config or Path.home() / '.local/state/espyspa-matter/hardware.json'
            cfg = json.loads(path.read_text()) if path.exists() else {}
        else:
            cfg = toolkit.config(args.config or toolkit.ROOT / '.hardware.local.json')
        url = os.environ.get('ESPYSPA_DIAGNOSTICS_URL', cfg.get('diagnostics_url', toolkit.hardware_env().get('ESPYSPA_DIAGNOSTICS_URL', '')))
        if args.local:
            result = safe_operate(dict(endpoint=endpoint, prop=args.property, raw_value=args.value, dry_run=args.dry_run, url=url))
        else:
            payload = dict(endpoint=endpoint, prop=args.property, raw_value=args.value, dry_run=args.dry_run, url=url)
            program = ('import sys,json; from pathlib import Path; sys.path.insert(0,str(Path.home()/".local/lib/espyspa-matter")); '
                       'import matter_write as w; p=json.load(sys.stdin); '
                       'r=w.safe_operate(p); print(json.dumps(r))')
            completed = subprocess.run(toolkit.ssh_argv(cfg, 'python3 -c ' + shlex.quote(program)),
                input=json.dumps(payload).encode(), capture_output=True, timeout=240, check=False)
            if completed.returncode or len(completed.stdout) > matter.LIMIT:
                raise ValueError('SSH write outcome unknown; inspect state before another command')
            result = json.loads(completed.stdout)
        print(json.dumps(toolkit.redact(result), sort_keys=True))
        return 0 if result.get('status') in ('dry_run_complete', 'confirmed') else 1
    except ValueError as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 2
    except (Exception, KeyboardInterrupt):
        print(json.dumps({'error': 'Matter write could not complete; inspect input, controller state and readback before retrying'}), file=sys.stderr)
        return 2


def safe_operate(payload):
    previous = signal.getsignal(signal.SIGALRM)
    def expired(signum, frame):
        raise OperationTimeout()
    try:
        signal.signal(signal.SIGALRM, expired)
        signal.alarm(180)
        return operate(**payload)
    except OperationTimeout:
        return {'status': 'uncertain', 'accepted': None, 'error': 'Operation timed out; inspect state before another command'}
    except ValueError as error:
        return {'status': 'failed', 'error': str(error)}
    except Exception:
        return {'status': 'failed', 'error': 'Write could not complete; verify discovery, controller state and RF before another command'}
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous)
