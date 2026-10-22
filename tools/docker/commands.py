"""Container-side local build commands; no hardware or release operations."""
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path('/workspace')
STATE = ROOT / '.local-build'
BUILD = STATE / 'build'
CONFIG = STATE / 'sdkconfig'
MATTER_REVISION = '4cedd4ad40821933d6cc237e80ab6604f2953fb0'
IDF_REVISION = '4c2820d377d1375e787bcef612f0c32c1427d183'


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def identity():
    revision = output('git', 'rev-parse', 'HEAD')
    dirty = bool(output('git', 'status', '--porcelain', '--untracked-files=normal'))
    commit = revision[:7] + ('-dirty' if dirty else '')
    return {'version': '0.1.0-dev.local+' + commit, 'channel': 'development',
            'commit': commit, 'revision': revision, 'dirty': dirty}


def run(args, log):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    with subprocess.Popen(list(map(str, args)), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True) as process:
        for line in process.stdout:
            print(line, end='', flush=True)
            log.write(line)
        code = process.wait()
    if code:
        raise subprocess.CalledProcessError(code, args)


def tests(log):
    run(['python3', '-m', 'unittest', 'discover', '-s', 'tests', '-p', '*_test.py', '-v'], log)
    for compiler in ('g++', 'clang++'):
        run(['env', 'CXX=' + compiler, 'tests/run.sh'], log)


def build(log, metrics):
    current = identity()
    os.environ.update(ESPYSPA_VERSION=current['version'], ESPYSPA_CHANNEL='development',
                      ESPYSPA_GIT_COMMIT=current['commit'], GITHUB_SHA=current['revision'] +
                      ('-dirty' if current['dirty'] else ''))
    metrics['identity'] = current
    print('Local firmware identity: ' + current['version'], flush=True)
    previous_path = STATE / 'identity.json'
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else None
    idf = ['idf.py', '-B', str(BUILD)]
    configured = (BUILD / 'CMakeCache.txt').is_file()
    if configured and previous != current:
        run(idf + ['reconfigure'], log)
    # Select target/config only when creating the build tree. Never clean it.
    initial = [] if configured else ['-D', 'SDKCONFIG=' + str(CONFIG)]
    run(['ccache', '--zero-stats'], log)
    started = time.monotonic()
    try:
        run(idf + initial + ['build'], log)
    finally:
        metrics['build_seconds'] = round(time.monotonic() - started, 3)
        run(['ccache', '--show-stats'], log)
        metrics['ccache'] = dict(line.split('\t', 1) for line in
                                 output('ccache', '--print-stats').splitlines())
    run(['python3', 'scripts/check_build.py', '--build', BUILD, '--config', CONFIG], log)
    previous_path.write_text(json.dumps(current, indent=2) + '\n')


def package(log, metrics):
    tests(log)
    build(log, metrics)
    destination = STATE / 'package'
    destination.mkdir(exist_ok=True)
    metadata = json.loads((BUILD / 'flasher_args.json').read_text())
    for name in list(metadata['flash_files'].values()) + [
            'flasher_args.json', 'quality-report.json', 'espyspa_matter.elf']:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(BUILD / name, target)
    # Package a copy: the public-name rewrite must not alter Ninja's metadata.
    run(['python3', 'scripts/package_firmware.py', '--build', destination], log)
    (destination / 'espyspa_matter.bin').unlink()
    current = metrics['identity']
    run(['python3', 'scripts/make_manifest.py', '--image', destination / 'espyspa-matter.bin',
         '--version', current['version'], '--channel', 'development', '--commit', current['commit'],
         '--url', 'https://local.invalid/espyspa-matter.bin',
         '--output', destination / 'manifest.json'], log)
    print('Local package: ' + str(destination), flush=True)


def main():
    os.chdir(ROOT)
    command = sys.argv[1]
    if output('git', '-C', os.environ['ESP_MATTER_PATH'], 'rev-parse', 'HEAD') != MATTER_REVISION:
        raise RuntimeError('SDK image has the wrong ESP-Matter revision; run prepare')
    idf_path = Path(os.environ['IDF_PATH'])
    version = (idf_path / 'tools/cmake/version.cmake').read_text()
    if (output('git', '-C', str(idf_path), 'rev-parse', 'HEAD') != IDF_REVISION or
            any('set(IDF_VERSION_' + part + ' ' + value + ')' not in version
                for part, value in [('MAJOR', '5'), ('MINOR', '4'), ('PATCH', '1')])):
        raise RuntimeError('SDK image has the wrong ESP-IDF version; run prepare')
    logs = STATE / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    metrics = {'command': command, 'timestamp': stamp, 'success': False,
               'jobs': int(os.environ['ESPYSPA_BUILD_JOBS']),
               'matter_revision': MATTER_REVISION, 'idf_version': 'v5.4.1', 'idf_revision': IDF_REVISION}
    start = time.monotonic()
    try:
        with (logs / (stamp + '-' + command + '.log')).open('w') as log:
            if command == 'build':
                build(log, metrics)
            elif command == 'package':
                package(log, metrics)
            elif command == 'test':
                tests(log)
            elif command == 'stats':
                run(['ccache', '--show-stats'], log)
            else:
                raise ValueError('Unsupported command: ' + command)
        metrics['success'] = True
    finally:
        metrics['total_seconds'] = round(time.monotonic() - start, 3)
        (logs / (stamp + '-' + command + '.json')).write_text(json.dumps(metrics, indent=2) + '\n')
        if command == 'package' and metrics['success']:
            (STATE / 'package/local-build.json').write_text(json.dumps(metrics, indent=2) + '\n')
        print(json.dumps(metrics, indent=2), flush=True)


if __name__ == '__main__':
    main()
