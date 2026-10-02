"""Conservative firmware selection and aggregate gate policy for GitHub Actions."""
import argparse
import os
from pathlib import PurePosixPath
import subprocess


def needs_firmware(paths):
    """Only explicitly recognised documentation paths may skip compilation."""
    for path in paths:
        p = PurePosixPath(path)
        if path in {"README.md", "LICENSE", "LICENSE.md"}:
            continue
        if p.parts[0] == "docs" and p.suffix in {".md", ".png", ".jpg", ".jpeg"}:
            continue
        return True
    return False


def gate_passes(changes, quality, protocol, required, firmware):
    if (changes, quality, protocol) != ("success", "success", "success"):
        return False
    return (required == "true" and firmware == "success") or (
        required == "false" and firmware == "skipped"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--gate", action="store_true")
    args = parser.parse_args()
    if args.gate:
        values = [os.environ.get(k, "") for k in (
            "CHANGES", "QUALITY", "PROTOCOL", "FIRMWARE_REQUIRED", "FIRMWARE"
        )]
        if not gate_passes(*values):
            parser.exit(1, f"Quality gate failed: {values}\n")
        print("Quality gate passed (firmware built or explicitly documentation-only).")
    else:
        if not args.base:
            parser.error("--base is required for change classification")
        # Disable rename detection so both the old and new path are checked.
        # Use the checked-out PR merge result against the PR's actual base SHA.
        diff = subprocess.check_output([
            "git", "diff", "--name-only", "--no-renames", "-z", args.base, args.head, "--"
        ])
        paths = [p.decode("utf-8", errors="surrogateescape") for p in diff.split(b"\0") if p]
        print(f"firmware={'true' if needs_firmware(paths) else 'false'}")


if __name__ == "__main__":
    main()
