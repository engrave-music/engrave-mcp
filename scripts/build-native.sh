#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
jobs="${1:-8}"
build_directory="${2:-build/xmake-linux}"
# Third positional argument optionally selects a custom engine checkout.
# cairo and zlib come from the Pixi environment (pixi::cairo, pixi::zlib).
engine_source="${3:-$PWD/vendor/engrave}"
# The pixi environment installs vendor/engrave as an editable package, so the
# submodule must exist before the first pixi command.
git submodule update --init --depth 1 -- vendor/engrave
pixi run --locked python scripts/bootstrap.py
pixi run --locked python scripts/prepare.py
pixi run --locked python scripts/prepare_xmake.py --check --engine-source "$engine_source"
pixi run --locked xmake f -y -m release -o "$build_directory" \
    --engine_source="$engine_source" \
    --cc="gcc@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-cc" \
    --cxx="g++@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-c++" \
    --ld="g++@$PWD/.pixi/envs/default/bin/x86_64-conda-linux-gnu-c++"
pixi run --locked xmake build -y -j "$jobs" engrave_mcp engrave_mcp_test registry_dump registry_check host_api_test
