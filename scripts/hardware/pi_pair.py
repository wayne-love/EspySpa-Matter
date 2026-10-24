"""Pi-local, one-attempt multi-admin pairing with non-echoing code input.

This separate operator tool is never invoked by spa-matter or over automated SSH.
Raw chip-tool output goes to an unnamed private temporary file and is discarded.
"""
import fcntl
import getpass
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile
import warnings

from spa_matter import LIMIT, REVISION, parse_report, private_path, request

NODE_ID = 0x45535041


def verify_readback(state):
    """Verify the existing identity and record it; never commission or retry."""
    private_path(state, directory=True)
    private_path(state / 'controller', directory=True)
    private_path(state / 'pairing-attempt.json')
    attempt = json.loads((state / 'pairing-attempt.json').read_text())
    if attempt.get('node_id') != NODE_ID or attempt.get('revision') != REVISION:
        raise ValueError('Existing attempt record does not match this controller')
    binary = Path.home() / '.local/lib/espyspa-matter/chip-tool'
    common = ['--storage-directory', str(state / 'controller'), '--commissioner-name', 'alpha']
    req = request(NODE_ID, 0, 0x1d, 3)
    status, text = capture([str(binary), 'any', 'read-by-id', '29', '3', str(NODE_ID), '0',
                            '--timeout', '15'] + common, state, 22)
    readback = parse_report(text, req, status)
    if readback['status'] != 'supported':
        return {'verified': False, 'readback': readback,
                'reason': 'Existing identity readback failed; do not commission again'}
    metadata = state / 'commissioned.json'
    record = {'node_id': NODE_ID, 'revision': REVISION}
    if metadata.exists():
        private_path(metadata)
        if json.loads(metadata.read_text()) != record:
            raise ValueError('Existing commissioned record differs; refusing to replace it')
    else:
        fd = os.open(metadata, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as output:
            json.dump(record, output)
    return {'verified': True, 'node_id': NODE_ID, 'root_parts_list': readback['value']}


def verify_existing(state):
    private_path(state, directory=True)
    lock_fd = os.open(state / 'controller.lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return verify_readback(state)


def fresh_code(text):
    if not re.fullmatch(r'[0-9 -]+', text):
        raise ValueError('Expected a fresh Apple Home manual Matter pairing code')
    text = text.replace(' ', '').replace('-', '')
    if len(text) not in (11, 21):
        raise ValueError('Expected an 11- or 21-digit Matter manual pairing code')
    return text


def capture(argv, state, timeout):
    def child_limits():
        os.umask(0o077)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_FSIZE, (LIMIT + 1, LIMIT + 1))

    with tempfile.TemporaryFile(dir=state) as output:
        try:
            result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout,
                                    preexec_fn=child_limits, check=False)
        except subprocess.TimeoutExpired:
            return -1, ''
        output.seek(0)
        raw = output.read(LIMIT + 1)
        if len(raw) > LIMIT:
            return -1, ''
        return result.returncode, raw.decode('utf-8', errors='replace')


def pair(code, state):
    """Called only after hidden terminal input; no retry or credential deletion."""
    code = fresh_code(code)
    storage = state / 'controller'
    private_path(state, directory=True)
    private_path(storage, directory=True)
    binary = Path.home() / '.local/lib/espyspa-matter/chip-tool'
    trust = Path.home() / '.local/lib/espyspa-matter/paa-root-certs'
    if not binary.is_file() or not trust.is_dir():
        raise ValueError('Controller installation or attestation trust store is missing')
    if (state / 'commissioned.json').exists() or any(storage.iterdir()):
        raise ValueError('Controller storage is already populated; diagnose before another pairing attempt')
    lock_fd = os.open(state / 'controller.lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Exclusive creation prevents repeated attempts even after partial failure.
        attempt_fd = os.open(state / 'pairing-attempt.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(attempt_fd, 'w') as attempt:
            json.dump({'node_id': NODE_ID, 'revision': REVISION, 'status': 'attempted'}, attempt)
        common = ['--storage-directory', str(storage), '--commissioner-name', 'alpha']
        argv = [str(binary), 'pairing', 'code', str(NODE_ID), code,
                '--use-only-onnetwork-discovery', '1', '--discover-once', '1',
                '--paa-trust-store-path', str(trust), '--timeout', '120'] + common
        status, text = capture(argv, state, 130)
        completed = status == 0 and 'Device commissioning completed with success' in text
        if not completed:
            # Only fixed stage labels leave this process; never echo raw errors.
            stages = {name: marker in text for name, marker in {
                'pase': 'PASE session established',
                'attestation': 'Performing next commissioning step: AttestationVerification',
                'noc': 'Performing next commissioning step: SendNOC',
                'operational_discovery': 'Performing next commissioning step: FindOperational',
            }.items()}
            return {'commissioned': False, 'reason': 'Commissioning did not confirm success; preserve storage and diagnose before retrying',
                    'exit_code': status, 'observed_stages': stages,
                    'error_codes': sorted(set(re.findall(r'CHIP Error (0x[0-9A-Fa-f]{8})', text)))[:8]}
        return dict(commissioned=True, **verify_readback(state))


def main():
    try:
        if sys.argv[1:] == ['--verify-existing']:
            result = verify_existing(Path.home() / '.local/state/espyspa-matter')
            print(json.dumps(result, sort_keys=True))
            return 0 if result.get('verified') else 1
        if len(sys.argv) != 1 or not sys.stdin.isatty():
            raise ValueError('Run this helper directly in the Pi user’s terminal, with no arguments')
        # Refuse getpass's fallback to echoed stdin if a terminal is unavailable.
        with warnings.catch_warnings():
            warnings.simplefilter('error', getpass.GetPassWarning)
            code = fresh_code(getpass.getpass('Fresh Apple Home Matter code (hidden): '))
        result = pair(code, Path.home() / '.local/state/espyspa-matter')
        print(json.dumps(result, sort_keys=True))
        return 0 if result.get('verified') else 1
    except (Exception, KeyboardInterrupt):
        # Exceptions can contain argv (and the code); never format them.
        print(json.dumps({'error': 'Pairing helper stopped. Preserve storage; diagnose before retrying. No raw output is shown.'}))
        return 2


if __name__ == '__main__':
    sys.exit(main())
