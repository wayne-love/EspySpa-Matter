"""Validate the generated configuration and flashing package before publication."""
import hashlib
import json
import os
from pathlib import Path

EXPECTED_CONFIG = {
    'CONFIG_IDF_TARGET': '"esp32c6"',
    'CONFIG_OPENTHREAD_ENABLED': 'y',
    'CONFIG_PARTITION_TABLE_CUSTOM': 'y',
    'CONFIG_PARTITION_TABLE_FILENAME': '"partitions.csv"',
    'CONFIG_ENABLE_OTA_REQUESTOR': 'y',
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y',
    'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE': 'y',
    'CONFIG_ENABLE_WIFI_STATION': 'n',
    'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG': 'y',
    'CONFIG_ESP_CONSOLE_SECONDARY_NONE': 'y',
    'CONFIG_SPA_RX_GPIO': '19',
    'CONFIG_SPA_TX_GPIO': '20',
    'CONFIG_SPA_LED_GPIO': '10',
    'CONFIG_SPA_RESET_GPIO': '9',
    'CONFIG_CHIP_PROJECT_CONFIG': '"main/CHIPProjectConfig.h"',
}
MIN_OTA_FREE = 64 * 1024


def check(build, config_path, partitions_path):
    build = Path(build).resolve()
    config = {}
    for line in Path(config_path).read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            config[key] = value
    for key, value in EXPECTED_CONFIG.items():
        actual = config.get(key, 'n')
        if actual != value:
            raise ValueError(f'{key}: expected {value}, got {actual}')
    metadata = json.loads((build / 'flasher_args.json').read_text())
    files = metadata['flash_files']
    required = {'espyspa_matter.bin', 'bootloader/bootloader.bin',
                'partition_table/partition-table.bin', 'ota_data_initial.bin'}
    if not required.issubset(files.values()):
        raise ValueError('Flashing metadata is missing required images')
    ranges, images = [], []
    for address, name in files.items():
        path = (build / name).resolve()
        if not path.is_relative_to(build) or not path.is_file():
            raise ValueError(f'Missing or unsafe flashing image: {name}')
        data = path.read_bytes()
        if not data:
            raise ValueError(f'Empty flashing image: {name}')
        start = int(address, 0)
        ranges.append((start, start + len(data), name))
        images.append({'file': name, 'offset': address, 'bytes': len(data),
                       'sha256': hashlib.sha256(data).hexdigest()})
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise ValueError(f'Overlapping flash images: {previous[2]} and {current[2]}')
    def size(value):
        value = value.strip()
        return int(value[:-1], 0) * {'K': 1024, 'M': 1024 * 1024}[value[-1]] if value[-1] in 'KM' else int(value, 0)
    slots = {}
    for line in Path(partitions_path).read_text().splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            fields = [v.strip() for v in line.split(',')]
            if fields[1] == 'app' and fields[2] in ('ota_0', 'ota_1'):
                slots[fields[2]] = (int(fields[3], 0), size(fields[4]))
    if set(slots) != {'ota_0', 'ota_1'}:
        raise ValueError('Both OTA slots are required')
    app = next(i for i in images if i['file'] == 'espyspa_matter.bin')
    if int(app['offset'], 0) != slots['ota_0'][0]:
        raise ValueError('Application flashing offset does not match ota_0')
    remaining = {name: capacity - app['bytes'] for name, (_, capacity) in slots.items()}
    if min(remaining.values()) < MIN_OTA_FREE:
        raise ValueError(f'Firmware must leave at least {MIN_OTA_FREE} bytes free in both OTA slots: {remaining}')
    return {'commit': os.environ.get('GITHUB_SHA', 'local'), 'images': images,
            'ota_free_bytes': remaining, 'configuration_checked': EXPECTED_CONFIG}


if __name__ == '__main__':
    report = check('build', 'sdkconfig', 'partitions.csv')
    Path('build/quality-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
