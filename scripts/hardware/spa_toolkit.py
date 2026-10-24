"""Read-only Pi/Thread diagnostics. Python standard library only."""
import argparse
import datetime
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
LIMIT = 1024 * 1024


class DiagnosticError(Exception):
    pass


def redact(value):
    if isinstance(value, dict):
        return {k: ('[redacted]' if re.search(r'password|credential|secret|token|private_key', k, re.I)
                    else redact(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value]
    if isinstance(value, str):
        value = re.sub(r'(?i)(password|token|secret|credential)\s*[:=]\s*\S+', r'\1=[redacted]', value)
        def address(match):
            text = match.group()
            try:
                ipaddress.ip_address(text)
                return '[address]'
            except ValueError:
                return text
        return re.sub(r'[0-9a-fA-F]*:[0-9a-fA-F:]+|\b(?:\d{1,3}\.){3}\d{1,3}\b', address, value)
    return value


ENV_SETTINGS = {
    'ESPYSPA_SSH_HOST', 'ESPYSPA_SSH_USER', 'ESPYSPA_SSH_KEY',
    'ESPYSPA_DIAGNOSTICS_URL', 'ESPYSPA_LOG_PATH', 'ESPYSPA_SPA_CONNECTED',
    'ESPYSPA_MATTER_NODE_ID',
    'ESPYSPA_SERIAL_DEVICE',
}


def hardware_env():
    """Read a deliberately small assignment grammar; never invoke a shell."""
    path = Path.home() / '.config/espyspa/hardware.env'
    try:
        with path.open(encoding='utf-8') as source:
            content = source.read(65537)
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeError):
        raise DiagnosticError('Cannot read hardware.env (expected UTF-8 text)') from None
    if len(content) > 65536:
        raise DiagnosticError('hardware.env exceeds 65536-character limit')
    values = {}
    for line_number, line in enumerate(content.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        def fail(reason):
            raise DiagnosticError(f'hardware.env line {line_number}: {reason}')
        match = re.fullmatch(r'(?:export[ \t]+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)', line)
        if not match:
            fail('expected KEY=value assignment')
        key, raw = match.groups()
        if key not in ENV_SETTINGS:
            fail('unsupported setting')
        if key in values:
            fail('duplicate setting')
        if any(c in raw for c in ('$','`', '\\')) or any(ord(c) < 32 for c in raw):
            fail('expansion, escapes and control characters are unsupported')
        if raw.startswith(('"', "'")):
            quote = raw[0]
            end = raw.find(quote, 1)
            if end < 0:
                fail('unterminated quoted value')
            value = raw[1:end]
            trailing = raw[end + 1:]
            if trailing and not re.fullmatch(r'[ \t]+(?:#.*)?', trailing):
                fail('unsupported text after quoted value')
        else:
            value = re.split(r'[ \t]+#', raw, maxsplit=1)[0].rstrip()
            if re.search(r"[\s\"';|&<>(){}]", value):
                fail('quote values containing spaces; shell syntax is unsupported')
        if key == 'ESPYSPA_SPA_CONNECTED' and value not in ('true', 'false'):
            fail('ESPYSPA_SPA_CONNECTED must be exactly true or false')
        values[key] = value
    return values


def config(path):
    defaults = hardware_env()
    values = {}
    if path.exists():
        try:
            values = json.loads(path.read_text())
        except (OSError, ValueError):
            raise DiagnosticError('Cannot read configuration JSON') from None
        if not isinstance(values, dict):
            raise DiagnosticError('Configuration must be a JSON object')
    for key in ('ssh_host', 'ssh_user', 'ssh_key', 'diagnostics_url', 'log_path'):
        values[key] = os.environ.get('ESPYSPA_' + key.upper(), values.get(key, defaults.get('ESPYSPA_' + key.upper(), '')))
    connected = os.environ.get('ESPYSPA_SPA_CONNECTED', defaults.get('ESPYSPA_SPA_CONNECTED', 'true'))
    if connected not in ('true', 'false'):
        raise DiagnosticError('ESPYSPA_SPA_CONNECTED must be exactly true or false')
    values['spa_connected'] = connected == 'true'
    values['log_path'] = values['log_path'] or '/var/log/espyspa/serial.log'
    for key in ('ssh_host', 'ssh_user'):
        if not isinstance(values[key], str) or not re.fullmatch(r'[a-zA-Z0-9_.:-]+', values[key]) or values[key].startswith('-'):
            raise DiagnosticError('Set a valid ESPYSPA_' + key.upper())
    if not isinstance(values['ssh_key'], str) or not values['ssh_key']:
        raise DiagnosticError('Set ESPYSPA_SSH_KEY to your existing private key path')
    values['ssh_key'] = os.path.expanduser(values['ssh_key'])
    if not isinstance(values['log_path'], str) or not values['log_path'].startswith('/') or '\n' in values['log_path']:
        raise DiagnosticError('Log path must be an absolute path')
    return values


def ssh_argv(cfg, command):
    return ['ssh', '-F', '/dev/null', '-i', cfg['ssh_key'], '-o', 'StrictHostKeyChecking=yes',
            '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'ConnectTimeout=5',
            '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2',
            cfg['ssh_user'] + '@' + cfg['ssh_host'], command]


def ssh(cfg, command):
    argv = ssh_argv(cfg, command)
    try:
        result = subprocess.run(argv, capture_output=True, timeout=30, check=False)
    except subprocess.TimeoutExpired:
        raise DiagnosticError('SSH operation timed out after 30 seconds') from None
    except OSError:
        raise DiagnosticError('Cannot execute ssh; verify OpenSSH is installed') from None
    if result.returncode:
        reasons = {255: 'SSH connection/authentication/host-key failure',
                   22: 'HTTP endpoint returned an error', 28: 'Remote HTTP timeout',
                   7: 'Remote HTTP connection failed', 63: 'Response exceeded size limit'}
        raise DiagnosticError(reasons.get(result.returncode, 'Remote read failed') +
                              ' (exit %d); check connection, permissions and endpoint configuration' % result.returncode)
    if len(result.stdout) > LIMIT:
        raise DiagnosticError('Response exceeded 1 MiB limit')
    return result.stdout.decode('utf-8', errors='replace')


def diagnostics(cfg):
    try:
        if not isinstance(cfg['diagnostics_url'], str):
            raise ValueError('URL must be text')
        url = urllib.parse.urlsplit(cfg['diagnostics_url'])
        valid = (url.scheme == 'http' and url.hostname and url.port in (None, 80)
                 and not url.username and not url.password and url.path == '/api/diagnostics'
                 and not url.query and not url.fragment)
    except (ValueError, TypeError):
        valid = False
    if not valid:
        raise DiagnosticError('Set ESPYSPA_DIAGNOSTICS_URL to http://<host>/api/diagnostics')
    command = ('curl -q --globoff --noproxy ' + shlex.quote('*') +
              ' --proto =http --connect-timeout 5 --max-time 20 --max-filesize 1048576 -fsS -- ' +
              shlex.quote(cfg['diagnostics_url']) + ' | head -c 1048577')
    raw = ssh(cfg, 'bash -o pipefail -c ' + shlex.quote(command))
    try:
        data = json.loads(raw)
    except ValueError:
        raise DiagnosticError('Diagnostics endpoint returned invalid JSON') from None
    if not isinstance(data, dict):
        raise DiagnosticError('Diagnostics endpoint must return a JSON object')
    for key in ('thread', 'matter', 'counters'):
        if key in data and not isinstance(data[key], dict):
            raise DiagnosticError('Invalid diagnostics object: ' + key)
    return data


def logs(cfg, lines):
    # Bound individual lines as well as line count; detect truncated responses locally.
    command = 'tail -n ' + str(lines) + ' -- ' + shlex.quote(cfg['log_path']) + ' | head -c 1048577'
    return ssh(cfg, 'bash -o pipefail -c ' + shlex.quote(command))


def health(data, spa_connected=True, serial_error=None):
    issues = []
    if spa_connected and data.get('fresh') is not True:
        issues.append('Spa RF state is stale or unavailable')
    if data.get('thread', {}).get('role') not in (2, 3, 4):
        issues.append('Thread is detached, disabled or unknown')
    fabrics = data.get('matter', {}).get('fabric_count')
    if type(fabrics) is not int or fabrics <= 0:
        issues.append('No Matter fabric reported')
    if serial_error:
        issues.append('Serial log unavailable: ' + serial_error)
    return {'spa_connected': spa_connected,
            'spa_status': 'expected_connected' if spa_connected else 'intentionally_disconnected',
            'serial_log': {'readable': serial_error is None, 'error': serial_error},
            'diagnostics': data,
            'healthy': not issues, 'issues': issues, 'firmware': data.get('firmware'),
            'uptime_ms': data.get('uptime_ms'), 'thread': data.get('thread'),
            'matter': data.get('matter'), 'fresh': data.get('fresh'),
            'state_age_ms': data.get('state_age_ms'), 'last_error': data.get('last_error'),
            'free_heap': data.get('free_heap'), 'minimum_free_heap': data.get('minimum_free_heap'),
            'counters': data.get('counters')}


def health_summary(result):
    """Format an assessed, redacted result without changing health policy."""
    def number(value, suffix=''):
        return f'{value:,}{suffix}' if type(value) is int and value >= 0 else 'Unknown'

    uptime = result.get('uptime_ms')
    duration = 'Unknown'
    if type(uptime) is int and uptime >= 0:
        days, seconds = divmod(uptime // 1000, 86400)
        hours, seconds = divmod(seconds, 3600)
        minutes, seconds = divmod(seconds, 60)
        duration = f'{days}d {hours:02d}h {minutes:02d}m {seconds:02d}s'
    role = (result.get('thread') or {}).get('role')
    role_name = {0: 'Disabled', 1: 'Detached', 2: 'Child', 3: 'Router', 4: 'Leader'}.get(role, 'Unknown') if type(role) is int else 'Unknown'
    readable = (result.get('serial_log') or {}).get('readable')
    serial = 'Readable' if readable is True else 'Unavailable' if readable is False else 'Unknown'
    connected = result.get('spa_connected')
    spa = ('Intentionally disconnected' if connected is False else
           'Expected connected' if connected is True else 'Unknown')
    healthy = result.get('healthy')
    lines = [
        'Overall health: ' + ('Healthy' if healthy is True else 'Unhealthy' if healthy is False else 'Unknown'),
        'Firmware: ' + str(result.get('firmware') or 'Unknown'),
        'Uptime: ' + duration,
        'Serial log: ' + serial,
        'Thread role: ' + role_name,
        'Matter fabrics: ' + number((result.get('matter') or {}).get('fabric_count')),
        'Free heap: ' + number(result.get('free_heap'), ' bytes'),
        'Spa: ' + spa,
    ]
    advice = {
        'Spa RF state is stale or unavailable': 'Spa RF unavailable; check the spa connection and serial wiring.',
        'Thread is detached, disabled or unknown': 'Thread unavailable; check the border router and Thread connectivity.',
        'No Matter fabric reported': 'No Matter fabric reported; check commissioning status.',
    }
    for issue in result.get('issues', []):
        if issue.startswith('Serial log unavailable:'):
            issue = 'Serial log unavailable; check the log path, read permissions and Pi SSH access.'
        lines.append('Issue: ' + advice.get(issue, issue))
    return '\n'.join(lines)


def main(tool=None, argv=None):
    tool = tool or Path(sys.argv[0]).name
    parser = argparse.ArgumentParser(description='Read-only Pi/Thread diagnostics; output is redacted.')
    parser.add_argument('--config', type=Path, default=ROOT / '.hardware.local.json')
    output = parser.add_mutually_exclusive_group()
    output.add_argument('--json', action='store_true', help='emit JSON (default for status, health and capture)')
    if tool == 'spa-health':
        output.add_argument('--summary', action='store_true', help='show concise human-readable health and troubleshooting advice instead of JSON')
    parser.add_argument('--lines', type=int, default=100, help='log lines, 1–1000')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'hardware-captures')
    args = parser.parse_args(argv)
    try:
        if not 1 <= args.lines <= 1000:
            raise DiagnosticError('--lines must be between 1 and 1000')
        cfg = config(args.config)
        if tool == 'spa-logs':
            result = {'logs': logs(cfg, args.lines)}
        elif tool == 'spa-status':
            result = diagnostics(cfg)
        elif tool == 'spa-health':
            data = diagnostics(cfg)
            serial_error = None
            try:
                if not logs(cfg, args.lines).strip():
                    serial_error = 'Serial log is empty'
            except DiagnosticError as exc:
                serial_error = str(exc)
            result = health(data, cfg.get('spa_connected', True), serial_error)
        elif tool == 'spa-capture':
            result = {'captured_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      'errors': {}}
            for name, read in (('diagnostics', lambda: diagnostics(cfg)), ('logs', lambda: logs(cfg, args.lines))):
                try:
                    result[name] = read()
                except DiagnosticError as exc:
                    result['errors'][name] = str(exc)
            if 'diagnostics' in result:
                result['health'] = health(
                    result['diagnostics'], cfg.get('spa_connected', True),
                    result['errors'].get('logs') or
                    ('Serial log is empty' if not result.get('logs', '').strip() else None))
            args.output_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
            destination = args.output_dir / ('spa-' + stamp + '.json')
            fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as output:
                json.dump(redact(result), output, indent=2)
                output.write('\n')
            result = {'capture': str(destination), 'errors': result['errors']}
        else:
            raise DiagnosticError('Unknown toolkit command')
        result = redact(result)
        if getattr(args, 'summary', False):
            print(health_summary(result))
        else:
            print(result['logs'] if tool == 'spa-logs' and not args.json else json.dumps(result, indent=2))
        return 1 if result.get('healthy') is False or result.get('errors') else 0
    except (DiagnosticError, OSError) as exc:
        message = str(exc) if isinstance(exc, DiagnosticError) else 'Local configuration or capture filesystem operation failed'
        print(json.dumps({'error': message}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
