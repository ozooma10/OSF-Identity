#!/usr/bin/env python3

import copy
import json
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_ROOT = REPO_ROOT / "fixtures" / "osf-identity"


def load_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


class PublicSchemaContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.package_schema = load_json(FIXTURE_ROOT / "package.schema.json")
        cls.metadata_schema = load_json(FIXTURE_ROOT / "preset-metadata.schema.json")
        Draft202012Validator.check_schema(cls.package_schema)
        Draft202012Validator.check_schema(cls.metadata_schema)
        cls.package_validator = Draft202012Validator(cls.package_schema)
        cls.metadata_validator = Draft202012Validator(cls.metadata_schema)

    def assertValid(self, validator: Draft202012Validator, instance: object) -> None:
        errors = sorted(validator.iter_errors(instance), key=lambda error: list(error.path))
        self.assertEqual([], errors, "\n".join(error.message for error in errors))

    def assertInvalid(self, validator: Draft202012Validator, instance: object) -> None:
        self.assertTrue(list(validator.iter_errors(instance)), instance)

    @staticmethod
    def assignment() -> dict[str, object]:
        return {
            "target": {
                "plugin": "Starfield.esm",
                "localFormId": "00005983",
            },
            "preset": "Sarah.npc",
        }

    def test_checked_in_examples_and_schema_examples_are_valid(self) -> None:
        for path in (
            FIXTURE_ROOT / "Packs" / "author.sarah-example" / "package.json",
            FIXTURE_ROOT / "Packs" / "project.community-example" / "package.json",
        ):
            with self.subTest(path=path):
                self.assertValid(self.package_validator, load_json(path))

        sidecar_example = (
            FIXTURE_ROOT
            / "Packs"
            / "project.community-example"
            / "Starfield.esm"
            / "0029A8EB.json.example"
        )
        self.assertValid(self.metadata_validator, load_json(sidecar_example))

        for example in self.package_schema["examples"]:
            with self.subTest(schema="package", example=example):
                self.assertValid(self.package_validator, example)
        for example in self.metadata_schema["examples"]:
            with self.subTest(schema="metadata", example=example):
                self.assertValid(self.metadata_validator, example)

    def test_package_defaults_and_independent_requirement_lists(self) -> None:
        valid_instances = [
            {"schemaVersion": 1},
            {"schemaVersion": 1, "requires": {}},
            {"schemaVersion": 1, "requires": {"plugins": ["Example.esm"]}},
            {
                "schemaVersion": 1,
                "requires": {"assets": ["Textures/Example/Hair.dds"]},
            },
            {"schemaVersion": 1, "assignments": [self.assignment()]},
            {
                "schemaVersion": 1,
                "assignments": [
                    {
                        **self.assignment(),
                        "requires": {"plugins": ["ExampleHairMod.esm"]},
                    }
                ],
            },
        ]
        for instance in valid_instances:
            with self.subTest(instance=instance):
                self.assertValid(self.package_validator, instance)

    def test_removed_scope_and_runtime_path_failures_are_rejected(self) -> None:
        scoped = self.assignment()
        scoped["scope"] = "faceAndBody"
        self.assertInvalid(
            self.package_validator,
            {"schemaVersion": 1, "assignments": [scoped]},
        )

        for preset in (
            "/Sarah.npc",
            "\\Sarah.npc",
            "C:\\Sarah.npc",
            "../Sarah.npc",
            "Presets/../Sarah.npc",
            "Presets\\..\\Sarah.npc",
            "Sarah.json",
        ):
            assignment = self.assignment()
            assignment["preset"] = preset
            with self.subTest(preset=preset):
                self.assertInvalid(
                    self.package_validator,
                    {"schemaVersion": 1, "assignments": [assignment]},
                )

        for asset in (
            "/Textures/Hair.dds",
            "\\Textures\\Hair.dds",
            "C:\\Textures\\Hair.dds",
            "../Textures/Hair.dds",
            "Textures/../Hair.dds",
            "Textures\\..\\Hair.dds",
        ):
            with self.subTest(asset=asset):
                self.assertInvalid(
                    self.package_validator,
                    {"schemaVersion": 1, "requires": {"assets": [asset]}},
                )

    def test_package_runtime_scalar_limits_are_encoded(self) -> None:
        invalid_instances = [
            {},
            {"schemaVersion": 2},
            {"schemaVersion": 1, "priority": "high"},
            {"schemaVersion": 1, "priority": 1000001},
            {"schemaVersion": 1, "assignments": []},
            {"schemaVersion": 1, "surprise": True},
            {"schemaVersion": 1, "requires": {"plugins": [".esm"]}},
            {"schemaVersion": 1, "requires": {"plugins": ["Example.txt"]}},
        ]
        bad_form_id = self.assignment()
        bad_form_id["target"] = copy.deepcopy(bad_form_id["target"])
        bad_form_id["target"]["localFormId"] = "01000000"
        invalid_instances.append({"schemaVersion": 1, "assignments": [bad_form_id]})

        for instance in invalid_instances:
            with self.subTest(instance=instance):
                self.assertInvalid(self.package_validator, instance)

    def test_preset_metadata_defaults_and_path_rules(self) -> None:
        for instance in (
            {"schemaVersion": 1},
            {"schemaVersion": 1, "requires": {}},
            {"schemaVersion": 1, "requires": {"plugins": ["Example.esm"]}},
            {
                "schemaVersion": 1,
                "requires": {"assets": ["Meshes/Example/Hair.mesh"]},
            },
        ):
            with self.subTest(instance=instance):
                self.assertValid(self.metadata_validator, instance)

        for instance in (
            {},
            {"schemaVersion": 2},
            {"schemaVersion": 1, "surprise": True},
            {"schemaVersion": 1, "requires": {"plugins": ["not-a-plugin"]}},
            {"schemaVersion": 1, "requires": {"assets": ["../Hair.dds"]}},
            {"schemaVersion": 1, "requires": {"assets": ["Hair.dds", "Hair.dds"]}},
        ):
            with self.subTest(instance=instance):
                self.assertInvalid(self.metadata_validator, instance)


if __name__ == "__main__":
    unittest.main()
