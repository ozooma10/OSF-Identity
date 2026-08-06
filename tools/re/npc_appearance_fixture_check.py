#!/usr/bin/env python3
"""Validate golden CK and CharGenMenu .npc fixture provenance and completeness."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "fixtures" / "osf-identity" / "Presets"
PRODUCERS = {"CK": "Creation Kit", "CharGenMenu": "CharGenMenu"}
FIXTURE_NAMES = (
    "Baseline.npc",
    "HeadpartOnly.npc",
    "FacialMorphOnly.npc",
    "TintOnly.npc",
    "BodyMorphOnly.npc",
    "Sarah.npc",
)
MAX_METADATA_BYTES = 1024 * 1024
MAX_PRESET_BYTES = 32 * 1024 * 1024
TARGET_GAME_VERSION = "1.16.244.0"
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
LOCAL_FORM_ID_RE = re.compile(r"^(?:[0-9A-Fa-f]{1,6}|0[0-9A-Fa-f]{6}|00[0-9A-Fa-f]{6})$")
PLUGIN_RE = re.compile(r"^[^\\/:]+\.[eE][sS][mMpPlL]$")


class DuplicateKeyError(ValueError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON property {key!r}")
        result[key] = value
    return result


def _exact_keys(value: dict[str, Any], required: set[str], optional: set[str], where: str) -> list[str]:
    errors = []
    missing = sorted(required - value.keys())
    unknown = sorted(value.keys() - required - optional)
    if missing:
        errors.append(f"{where}: missing properties: {', '.join(missing)}")
    if unknown:
        errors.append(f"{where}: unknown properties: {', '.join(unknown)}")
    return errors


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_producer(root: Path, directory_name: str, producer_name: str) -> list[str]:
    errors: list[str] = []
    producer_dir = root / directory_name
    label = directory_name
    if not producer_dir.is_dir():
        return [f"{label}: producer directory is missing: {producer_dir}"]

    metadata_path = producer_dir / "metadata.json"
    if not metadata_path.is_file():
        return [f"{label}: metadata.json is missing"]
    try:
        metadata_size = metadata_path.stat().st_size
        if metadata_size <= 0 or metadata_size > MAX_METADATA_BYTES:
            return [f"{label}: metadata.json size {metadata_size} is outside 1..{MAX_METADATA_BYTES}"]
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"), object_pairs_hook=_strict_object)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, DuplicateKeyError) as exc:
        return [f"{label}: metadata.json could not be read strictly: {exc}"]
    if not isinstance(metadata, dict):
        return [f"{label}: metadata root must be an object"]

    errors.extend(
        _exact_keys(
            metadata,
            {
                "schemaVersion",
                "producer",
                "producerVersion",
                "gameVersion",
                "sourceNpc",
                "loadedPlugins",
                "files",
            },
            {"$schema"},
            f"{label} metadata",
        )
    )
    if metadata.get("schemaVersion") != 1:
        errors.append(f"{label}: schemaVersion must be 1")
    if metadata.get("producer") != producer_name:
        errors.append(f"{label}: producer must be {producer_name!r}")
    producer_version = metadata.get("producerVersion")
    if not _is_nonempty_string(producer_version) or producer_version.strip().casefold() in {
        "exact version",
        "unknown",
    }:
        errors.append(f"{label}: producerVersion must be the exact non-placeholder version")
    if metadata.get("gameVersion") != TARGET_GAME_VERSION:
        errors.append(f"{label}: gameVersion must be {TARGET_GAME_VERSION!r}")

    source = metadata.get("sourceNpc")
    if not isinstance(source, dict):
        errors.append(f"{label}: sourceNpc must be an object")
    else:
        errors.extend(_exact_keys(source, {"plugin", "localFormId", "editorId"}, set(), f"{label} sourceNpc"))
        if not isinstance(source.get("plugin"), str) or not PLUGIN_RE.fullmatch(source["plugin"]):
            errors.append(f"{label}: sourceNpc.plugin is invalid")
        if not isinstance(source.get("localFormId"), str) or not LOCAL_FORM_ID_RE.fullmatch(
            source["localFormId"]
        ):
            errors.append(f"{label}: sourceNpc.localFormId is invalid")
        if not _is_nonempty_string(source.get("editorId")):
            errors.append(f"{label}: sourceNpc.editorId must be a non-empty string")

    plugins = metadata.get("loadedPlugins")
    if not isinstance(plugins, list) or not plugins:
        errors.append(f"{label}: loadedPlugins must be a non-empty array")
    else:
        seen_plugins: set[str] = set()
        for index, plugin in enumerate(plugins):
            if not isinstance(plugin, str) or not PLUGIN_RE.fullmatch(plugin):
                errors.append(f"{label}: loadedPlugins[{index}] is invalid")
                continue
            folded = plugin.casefold()
            if folded in seen_plugins:
                errors.append(f"{label}: loadedPlugins contains duplicate {plugin!r}")
            seen_plugins.add(folded)

    files = metadata.get("files")
    if not isinstance(files, dict):
        errors.append(f"{label}: files must be an object")
        files = {}
    else:
        errors.extend(_exact_keys(files, set(FIXTURE_NAMES), set(), f"{label} files"))

    actual_npc_names = {path.name for path in producer_dir.glob("*.npc")}
    unexpected = sorted(actual_npc_names - set(FIXTURE_NAMES))
    if unexpected:
        errors.append(f"{label}: unexpected .npc files: {', '.join(unexpected)}")

    producer_root = producer_dir.resolve()
    for name in FIXTURE_NAMES:
        path = producer_dir / name
        entry = files.get(name)
        if not isinstance(entry, dict):
            if name in files:
                errors.append(f"{label} {name}: metadata entry must be an object")
        else:
            errors.extend(
                _exact_keys(
                    entry,
                    {"controlledEdit", "roundTripReloaded", "sha256"},
                    set(),
                    f"{label} {name}",
                )
            )
            if not _is_nonempty_string(entry.get("controlledEdit")):
                errors.append(f"{label} {name}: controlledEdit must be non-empty")
            if entry.get("roundTripReloaded") is not True:
                errors.append(f"{label} {name}: roundTripReloaded must be true")
            if not isinstance(entry.get("sha256"), str) or not SHA256_RE.fullmatch(entry["sha256"]):
                errors.append(f"{label} {name}: sha256 must be exactly 64 hexadecimal digits")

        if not path.is_file():
            errors.append(f"{label} {name}: fixture file is missing or not regular")
            continue
        try:
            resolved = path.resolve(strict=True)
            if resolved.parent != producer_root:
                errors.append(f"{label} {name}: resolved path escapes the producer directory")
                continue
            size = resolved.stat().st_size
            if size <= 0 or size > MAX_PRESET_BYTES:
                errors.append(f"{label} {name}: size {size} is outside 1..{MAX_PRESET_BYTES}")
                continue
            if isinstance(entry, dict) and isinstance(entry.get("sha256"), str) and SHA256_RE.fullmatch(
                entry["sha256"]
            ):
                actual_hash = _sha256(resolved)
                if actual_hash.casefold() != entry["sha256"].casefold():
                    errors.append(
                        f"{label} {name}: SHA-256 mismatch (metadata {entry['sha256']}, actual {actual_hash})"
                    )
        except OSError as exc:
            errors.append(f"{label} {name}: could not inspect fixture: {exc}")
    return errors


def validate_root(root: Path) -> list[str]:
    errors: list[str] = []
    for directory_name, producer_name in PRODUCERS.items():
        errors.extend(validate_producer(root, directory_name, producer_name))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--json", action="store_true", help="emit a machine-readable readiness report")
    args = parser.parse_args()

    root = args.root.resolve()
    errors = validate_root(root)
    report = {"root": str(root), "ready": not errors, "errors": errors}
    if args.json:
        print(json.dumps(report, indent=2))
    elif errors:
        print(f"NPC appearance fixture gate: NOT READY ({len(errors)} error(s))")
        for error in errors:
            print(f"- {error}")
    else:
        print("NPC appearance fixture gate: READY (12/12 producer-round-tripped files verified)")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
