#!/usr/bin/env bash
# Host-side verification for non-Windows development machines.
#
# Runs everything from tools/verify.ps1 that does not require the MSVC/xmake
# game-plugin toolchain: both host test suites (built with $CXX, default g++),
# the public JSON Schema contracts, fixture-validator tests, fixture provenance
# gate, and whitespace check. The game plugin still requires the Windows build.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

cxx="${CXX:-g++}"
build_dir="${1:-build/host}"
mkdir -p "$build_dir"

cxxflags=(-std=c++23 -Wall -Wextra -Werror -Isrc)

echo "[verify-host] building host test suites with $cxx"
"$cxx" "${cxxflags[@]}" \
    tools/tests/npc_appearance_config_tests.cpp \
    src/NpcAppearance/Config.cpp \
    src/NpcAppearance/ConfigDetail.cpp \
    src/NpcAppearance/ManifestParser.cpp \
    src/NpcAppearance/PackageDiscovery.cpp \
    src/NpcAppearance/Selection.cpp \
    src/NpcAppearance/Json.cpp \
    -o "$build_dir/npc-appearance-config-tests"
"$cxx" "${cxxflags[@]}" \
    tools/tests/npc_appearance_preset_tests.cpp \
    src/NpcAppearance/Preset.cpp \
    src/NpcAppearance/Json.cpp \
    -o "$build_dir/npc-appearance-preset-tests"

echo "[verify-host] running host test suites"
"$build_dir/npc-appearance-config-tests"
"$build_dir/npc-appearance-preset-tests"

echo "[verify-host] running JSON and fixture validator tests"
python3 -m unittest \
    tools.tests.npc_appearance_fixture_check_tests \
    tools.tests.npc_appearance_schema_tests

echo "[verify-host] running fixture provenance gate"
python3 tools/re/npc_appearance_fixture_check.py

if [ -d .git ]; then
    echo "[verify-host] git diff --check"
    git diff --check
fi

echo "[verify-host] all host-side OSF Identity checks passed"
