#!/usr/bin/env bash
# Runs inside the toolchain container. Project is bind-mounted at /work.
set -euo pipefail

CONFIG="${1:-release}"
cd /work

premake5 gmake --file=premake5.lua
make -C build "config=${CONFIG}" CXX="${CXX}" CC="${CC}" AR=ar -j"$(nproc)"

echo
echo "Output:"
ls -la /work/output/*/ 2>/dev/null || true
