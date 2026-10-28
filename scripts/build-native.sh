#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
jobs="${1:-8}"
build_directory="${2:-build/xmake-linux}"
pixi run --locked -e native python scripts/prepare.py
pixi run --locked -e native python scripts/bootstrap.py
pixi run --locked -e native python scripts/prepare_xmake.py --check
pixi run --locked -e native xmake f -y -m release -o "$build_directory" \
    --cc="gcc@$PWD/.pixi/envs/native/bin/x86_64-conda-linux-gnu-cc" \
    --cxx="g++@$PWD/.pixi/envs/native/bin/x86_64-conda-linux-gnu-c++" \
    --ld="g++@$PWD/.pixi/envs/native/bin/x86_64-conda-linux-gnu-c++"
pixi run --locked -e native xmake build -y -j "$jobs" musescore_mcp
