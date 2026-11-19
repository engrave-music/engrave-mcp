#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
jobs="${1:-8}"
engine_source="${3:-$PWD/vendor/engrave}"
profile="${4:-release}"
build_directory="${2:-build/$profile}"
# The pixi environment installs vendor/engrave as an editable package, so the
# submodule must exist before the first pixi command.
git submodule update --init --depth 1 -- vendor/engrave
pixi run --locked python scripts/bootstrap.py
pixi run --locked python scripts/prepare.py
pixi run --locked python scripts/check_build_inputs.py --check --engine-source "$engine_source"
pixi run --locked python scripts/bbuild.py --profile "$profile" --build-directory "$build_directory" -j "$jobs" build engrave_mcp engrave_mcp_test registry_dump registry_check host_api_test
