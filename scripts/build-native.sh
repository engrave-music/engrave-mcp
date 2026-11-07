#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
jobs="${1:-8}"
build_directory="${2:-build/xmake-linux}"
cairo_prefix="${3:-}"
# Fourth positional argument optionally selects a custom engine checkout.
engine_source="${4:-$PWD/vendor/engrave}"
pixi run --locked python scripts/bootstrap.py
pixi run --locked python scripts/prepare.py
pixi run --locked python scripts/prepare_xmake.py --check --engine-source "$engine_source"
extra_config=()
if [[ -n "$cairo_prefix" ]]; then
    extra_config+=("--cairo_prefix=$cairo_prefix")
fi
pixi run --locked xmake f -y -m release -o "$build_directory" \
    --engine_source="$engine_source" \
    --cc="gcc@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-cc" \
    --cxx="g++@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-c++" \
    --ld="g++@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-c++" "${extra_config[@]}"
pixi run --locked xmake build -y -j "$jobs" engrave_mcp engrave_mcp_test registry_dump registry_check host_api_test
