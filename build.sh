#!/usr/bin/env bash
# Builds MapExpander.SA.asi inside Docker (Linux/macOS/WSL host).
#   ./build.sh          -> Release
#   ./build.sh debug    -> Debug
set -euo pipefail
cd "$(dirname "$0")"
docker build -t map-expander-toolchain -f Dockerfile .
docker run --rm -v "$PWD:/work" map-expander-toolchain "${1:-release}"
