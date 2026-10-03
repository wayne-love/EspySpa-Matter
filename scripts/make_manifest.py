"""Create the OTA manifest consumed by EspySpa devices."""
import argparse
import hashlib
import json
from pathlib import Path


def make_manifest(image, version, channel, commit, url, target="esp32c6"):
    image = Path(image)
    data = image.read_bytes()
    if channel not in {"release", "development"}:
        raise ValueError("channel must be release or development")
    if not version:
        raise ValueError("version is required")
    if not url.startswith("https://"):
        raise ValueError("firmware URL must use https")
    return {
        "version": version,
        "channel": channel,
        "commit": commit,
        "target": target,
        "url": url,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", choices=("release", "development"), required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    manifest = make_manifest(args.image, args.version, args.channel, args.commit, args.url)
    Path(args.output).write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
