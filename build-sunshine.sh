#!/bin/bash
set -euo pipefail

podman run --rm -it --privileged \
  -v "$PWD:/workspace:Z" \
  -w /workspace \
  -e CLONE_URL="https://github.com/XT-Martinez/Sunshine" \
  -e BRANCH="vulkan-pr-4" \
  sunshine-flatpak-builder \
  bash -lc "./build-flatpak-container.sh"
