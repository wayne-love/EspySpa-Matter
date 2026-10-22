#!/usr/bin/env bash
set -eo pipefail
# SDK exports are provided by the pinned image; no runtime installation.
source "$IDF_PATH/export.sh"
source "$ESP_MATTER_PATH/export.sh"
export ESPYSPA_NINJA="$(command -v ninja)"
export PATH="/workspace/tools/docker/bin:$PATH"
exec python3 /workspace/tools/docker/commands.py "$@"
