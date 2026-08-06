# Author Guide

## Package layout

Each package is an independent directory under
`Data/SFSE/Plugins/NpcAppearanceLoader/Packages`:

```text
author.package-id/
  package.json
  Presets/
    Sarah.npc
```

Do not bundle the framework DLL in an appearance package.

## Manifest

```json
{
  "$schema": "../../package.schema.json",
  "schemaVersion": 1,
  "packageId": "author.sarah-example",
  "priority": 100,
  "requires": {
    "plugins": ["Starfield.esm"],
    "assets": []
  },
  "assignments": [
    {
      "target": {
        "plugin": "Starfield.esm",
        "localFormId": "00005983"
      },
      "preset": "Presets/Sarah.npc",
      "scope": "faceAndBody"
    }
  ]
}
```

Fields:

- `packageId`: stable lowercase identifier, 3-128 characters;
- `priority`: higher values win, from `-1000000` through `1000000`;
- `requires.plugins`: every required ESM/ESP/ESL must be loaded;
- `requires.assets`: Data-relative VFS paths required by the appearance;
- `target.plugin`: plugin that owns the base NPC;
- `target.localFormId`: plugin-local base FormID, never a load-order-prefixed
  runtime FormID;
- `preset`: package-relative `.npc` path contained under the package directory;
- `scope`: currently exactly `faceAndBody`.

Duplicate targets inside one package, duplicate package IDs, path traversal,
absolute preset paths, unknown properties, and missing dependencies are rejected.

## Supported targets

Version 0.1 supports unique `HumanRace` NPC bases. It does not support leveled or
template-shared generic crowds, race/sex conversion, or different appearances
for multiple references sharing one base.

Test at least one complete load, cell return or quickload, package removal, and
exact original restoration for every target you publish.

## Preset production

Supported inputs are:

- Creation Kit 1.16.244 `.npc` exports;
- CharGenMenu/SFEE `.npc` exports from the verified 1.16.244-compatible build.

The producer must successfully reload the exact final file. Do not hand-author,
reformat, line-normalize, or splice `.npc` bytes. Keep the original export as a
binary artifact.

CharGenMenu writes an empty `NPCFormEditorID` and uses a quantized tint encoding;
the framework detects and normalizes that declared producer contract. Merely
renaming arbitrary JSON to `.npc` does not make it compatible.

## Required assets

List every non-plugin Data asset the preset needs in `requires.assets`, using a
Data-relative path such as:

```json
"assets": ["Meshes/Actors/Human/FaceGenData/Example.mesh"]
```

The package remains disabled if any declared asset is absent from the running
game's VFS. A source-tree file is not proof that the installed VFS can resolve it.

## Validation

Use `fixtures/npc-appearance-loader/package.schema.json` for editor validation.
The runtime parser remains authoritative and intentionally rejects unknown
properties.
