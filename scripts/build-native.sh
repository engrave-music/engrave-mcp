#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
jobs="${1:-8}"
build_directory="${2:-build/native-linux}"
pixi run --locked -e native python scripts/prepare.py
pixi run --locked -e native cmake -S . -B "$build_directory" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/.pixi/envs/native"
pixi run --locked -e native cmake --build "$build_directory" --parallel "$jobs"
