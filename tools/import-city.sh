#!/usr/bin/env bash
# Builds a MapExpander region folder from a GTA III / Vice City install, in a throwaway container.
#   tools/import-city.sh "/games/GTA Vice City" "/games/GTA San Andreas" [region] [--no-rename]
set -euo pipefail
src="$1"; sa="$2"; region="${3:-vicecity}"; shift 3 2>/dev/null || shift $#
tools="$(cd "$(dirname "$0")" && pwd)"
docker run --rm -v "$src:/src:ro" -v "$sa:/sa" -v "$tools:/tools:ro" python:3-slim \
    sh -c "pip install -q pillow >/dev/null 2>&1; python /tools/import_city.py /src /sa --region=$region $*"
