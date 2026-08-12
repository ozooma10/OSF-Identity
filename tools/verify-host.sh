#!/usr/bin/env bash
# Host-side verification for non-Windows development machines.
#
# Runs everything from tools/verify.ps1 that does not require the MSVC/xmake
# game-plugin toolchain: both host test suites (built with $CXX, default g++),
# fixture-validator tests, the fixture provenance gate, and whitespace check.
# The game plugin still requires the Windows build.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

cxx="${CXX:-g++}"
build_dir="${1:-build/host}"
mkdir -p "$build_dir"

# Glaze is header-only and normally supplied by xmake; fetch the pinned tag
# for host builds that have no game toolchain.
glaze_dir="$build_dir/glaze"
if [ ! -d "$glaze_dir/include/glaze" ]; then
    echo "[verify-host] fetching glaze v7.0.2"
    rm -rf "$glaze_dir"
    git clone --depth 1 --branch v7.0.2 https://github.com/stephenberry/glaze "$glaze_dir"
fi

cxxflags=(-std=c++23 -Wall -Wextra -Werror -Isrc -isystem "$glaze_dir/include")

echo "[verify-host] building host test suites with $cxx"
"$cxx" "${cxxflags[@]}" \
    tools/tests/npc_appearance_config_tests.cpp \
    src/Config/AssignmentSelection.cpp \
    src/Config/ConfigDetail.cpp \
    src/Config/PackDiscovery.cpp \
    src/Config/RuntimeFormID.cpp \
    -o "$build_dir/npc-appearance-config-tests"
"$cxx" "${cxxflags[@]}" \
    tools/tests/npc_appearance_preset_tests.cpp \
    src/Config/Preset.cpp \
    -o "$build_dir/npc-appearance-preset-tests"

echo "[verify-host] running host test suites"
"$build_dir/npc-appearance-config-tests"
"$build_dir/npc-appearance-preset-tests"

echo "[verify-host] running fixture validator tests"
python3 -m unittest tools.tests.npc_appearance_fixture_check_tests

echo "[verify-host] running fixture provenance gate"
python3 tools/re/npc_appearance_fixture_check.py

if [ -d .git ]; then
    echo "[verify-host] git diff --check"
    git diff --check
fi

echo "[verify-host] all host-side OSF Identity checks passed"
