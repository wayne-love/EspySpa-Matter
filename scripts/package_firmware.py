"""Prepare the user-facing flashing package from ESP-IDF build outputs."""
import argparse
import json
import shutil
from pathlib import Path

INTERNAL_APP_NAME = "espyspa_matter.bin"
PUBLIC_APP_NAME = "espyspa-matter.bin"


def package_firmware(build_dir):
    build = Path(build_dir)
    source = build / INTERNAL_APP_NAME
    target = build / PUBLIC_APP_NAME
    flasher_args = build / "flasher_args.json"

    if not source.is_file() or source.stat().st_size == 0:
        raise ValueError(f"Missing or empty application image: {source}")
    if not flasher_args.is_file():
        raise ValueError(f"Missing flashing metadata: {flasher_args}")

    metadata = json.loads(flasher_args.read_text())
    flash_files = metadata.get("flash_files")
    if not isinstance(flash_files, dict):
        raise ValueError("flasher_args.json is missing flash_files")

    matches = [offset for offset, name in flash_files.items() if name == INTERNAL_APP_NAME]
    if len(matches) != 1:
        raise ValueError(
            f"Expected exactly one {INTERNAL_APP_NAME} entry in flash_files, found {len(matches)}"
        )

    shutil.copyfile(source, target)
    if target.stat().st_size != source.stat().st_size:
        raise ValueError("Packaged application image size does not match build output")

    flash_files[matches[0]] = PUBLIC_APP_NAME
    flasher_args.write_text(json.dumps(metadata, indent=2) + "\n")

    return {
        "source": str(source),
        "image": str(target),
        "flasher_args": str(flasher_args),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    args = parser.parse_args()
    package_firmware(args.build)


if __name__ == "__main__":
    main()
