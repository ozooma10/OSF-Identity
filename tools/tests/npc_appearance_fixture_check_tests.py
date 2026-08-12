#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_RE = Path(__file__).resolve().parents[1] / "re"
REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_ROOT = REPO_ROOT / "fixtures" / "osf-identity"
sys.path.insert(0, str(TOOLS_RE))

import npc_appearance_fixture_check as fixture_check  # noqa: E402


class NpcAppearanceFixtureCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "Presets"
        self._write_valid_matrix()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _write_valid_matrix(self) -> None:
        for directory_name, producer_name in fixture_check.PRODUCERS.items():
            producer_dir = self.root / directory_name
            producer_dir.mkdir(parents=True)
            file_metadata = {}
            for name in fixture_check.FIXTURE_NAMES:
                data = f"{directory_name}:{name}:golden-test-data".encode()
                (producer_dir / name).write_bytes(data)
                file_metadata[name] = {
                    "controlledEdit": "Unchanged baseline." if name == "Baseline.npc" else f"Controlled edit for {name}.",
                    "roundTripReloaded": True,
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            metadata = {
                "$schema": "../fixture-metadata.schema.json",
                "schemaVersion": 1,
                "producer": producer_name,
                "producerVersion": "1.16.244.0" if directory_name == "CK" else "1.0.0-test",
                "gameVersion": "1.16.244.0",
                "sourceNpc": {
                    "plugin": "Starfield.esm",
                    "localFormId": "00005983",
                    "editorId": "SarahMorgan",
                },
                "loadedPlugins": ["Starfield.esm"],
                "files": file_metadata,
            }
            (producer_dir / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")

    def _metadata(self, producer: str) -> tuple[Path, dict]:
        path = self.root / producer / "metadata.json"
        return path, json.loads(path.read_text(encoding="utf-8"))

    def test_complete_matrix_is_ready(self) -> None:
        self.assertEqual([], fixture_check.validate_root(self.root))

    def test_companion_smoke_pack_is_directly_loadable(self) -> None:
        plugin_directory = (
            FIXTURE_ROOT / "Packs" / "osf.identity-companion-smoke" / "Starfield.esm"
        )
        self.assertEqual(
            {"00005983.npc", "000059A7.npc"},
            {path.name for path in plugin_directory.glob("*.npc")},
        )
        expected_sources = {
            "00005983.npc": "HeadpartOnly.npc",
            "000059A7.npc": "Sarah.npc",
        }
        for target, source in expected_sources.items():
            with self.subTest(target=target, source=source):
                self.assertEqual(
                    (FIXTURE_ROOT / "Presets" / "CK" / source).read_bytes(),
                    (plugin_directory / target).read_bytes(),
                )

    def test_missing_file_is_rejected(self) -> None:
        (self.root / "CK" / "TintOnly.npc").unlink()
        errors = fixture_check.validate_root(self.root)
        self.assertTrue(any("CK TintOnly.npc: fixture file is missing" in error for error in errors))

    def test_hash_mismatch_is_rejected(self) -> None:
        (self.root / "CharGenMenu" / "Sarah.npc").write_bytes(b"changed after metadata")
        errors = fixture_check.validate_root(self.root)
        self.assertTrue(any("CharGenMenu Sarah.npc: SHA-256 mismatch" in error for error in errors))

    def test_failed_round_trip_is_rejected(self) -> None:
        path, metadata = self._metadata("CK")
        metadata["files"]["HeadpartOnly.npc"]["roundTripReloaded"] = False
        path.write_text(json.dumps(metadata), encoding="utf-8")
        errors = fixture_check.validate_root(self.root)
        self.assertIn("CK HeadpartOnly.npc: roundTripReloaded must be true", errors)

    def test_wrong_game_version_is_rejected(self) -> None:
        path, metadata = self._metadata("CharGenMenu")
        metadata["gameVersion"] = "1.16.242.0"
        path.write_text(json.dumps(metadata), encoding="utf-8")
        errors = fixture_check.validate_root(self.root)
        self.assertIn("CharGenMenu: gameVersion must be '1.16.244.0'", errors)

    def test_unknown_metadata_property_is_rejected(self) -> None:
        path, metadata = self._metadata("CK")
        metadata["unprovenClaim"] = True
        path.write_text(json.dumps(metadata), encoding="utf-8")
        errors = fixture_check.validate_root(self.root)
        self.assertIn("CK metadata: unknown properties: unprovenClaim", errors)

    def test_duplicate_json_property_is_rejected(self) -> None:
        path = self.root / "CK" / "metadata.json"
        path.write_text('{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8")
        errors = fixture_check.validate_root(self.root)
        self.assertTrue(any("duplicate JSON property 'schemaVersion'" in error for error in errors))

    def test_unexpected_preset_is_rejected(self) -> None:
        (self.root / "CK" / "Extra.npc").write_bytes(b"extra")
        errors = fixture_check.validate_root(self.root)
        self.assertIn("CK: unexpected .npc files: Extra.npc", errors)

    def test_empty_preset_is_rejected(self) -> None:
        (self.root / "CK" / "Baseline.npc").write_bytes(b"")
        errors = fixture_check.validate_root(self.root)
        self.assertTrue(any("CK Baseline.npc: size 0 is outside" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
