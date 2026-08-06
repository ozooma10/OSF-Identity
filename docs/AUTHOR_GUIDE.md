# Author Guide

## Quickstart

An appearance package replaces the face and body of unique human NPCs with presets you export from Creation Kit or CharGenMenu. It is a plain mod folder containing one manifest plus one preset file per target NPC:

```text
Data/SFSE/Plugins/OSFIdentity/Packages/
  author.my-pack/
    package.json                  # id, priority, shared dependencies
    Presets/
      Starfield.esm/              # plugin that owns the base NPC
        00005983.npc              # that plugin's local FormID (Sarah)
        00005983.identity.json    # optional per-preset dependencies
```

To build one:

1. **Export a preset** with Creation Kit `1.16.244` or the verified
   CharGenMenu/SFEE `1.16.244` build, and confirm the producer reloads the
   exact export. Never hand-edit the `.npc`.
2. **Name it after the target.** Directory = owning plugin, filename = that
   plugin's local FormID (8-digit hex, load-order index zeroed out). Presets
   must sit directly in the plugin directory — nested folders are not scanned.
3. **Write `package.json`** at the package root:

   ```json
   {
     "schemaVersion": 1,
     "packageId": "author.my-pack",
     "priority": 100,
     "requires": { "plugins": [], "assets": [] },
     "presetConvention": "pluginFolderLocalFormId"
   }
   ```

4. **Declare dependencies.** Optional hair, beard, or asset mods go in the
   manifest `requires` (package-wide) or a sidecar (per preset, below).
   Undeclared dependencies are the top cause of "works on my machine."
5. **Restart Starfield through SFSE.** Packages are scanned once after game
   data load; a quickload will not pick up changes. Confirm your `packageId`
   appears as the winner in
   `Documents\My Games\Starfield\SFSE\Logs\osf-identity.log` — failures are
   silent in-game and explained only in the log.


## Manifest reference

- `packageId` - stable lowercase identifier, 3-128 characters.
- `priority` - `-1000000` to `1000000`. When several valid packages target the
  same NPC, the highest priority wins; ties go to ascending `packageId`.
  Mod-manager order is irrelevant.
- `requires` - plugins and loose Data-relative assets every preset needs. The
  owning plugin of each preset is always an implicit requirement. A missing
  package-wide requirement disables the whole package.
- Exactly one of `presetConvention: "pluginFolderLocalFormId"` or
  `assignments`.

`package.schema.json` and `preset-metadata.schema.json` give editor
validation; the runtime parser is authoritative and rejects unknown
properties, duplicate targets, path traversal, and absolute preset paths.

## Per-preset dependencies

If a single preset needs optional mods, add a sidecar with the same filename
stem:

```json
{
  "schemaVersion": 1,
  "requires": {
    "plugins": ["ExampleHairMod.esm"],
    "assets": []
  }
}
```

Sidecar, manifest, and implicit owning-plugin requirements are additive. A
missing or malformed sidecar disables only its preset; the rest of the package
and lower-priority candidates still compete. List every plugin needed to
resolve non-vanilla headparts and every loose asset that needs an availability
check.

## Explicit assignments

Instead of `presetConvention`, advanced packages may map targets to presets
directly, with per-assignment `requires`:

```json
{
  "schemaVersion": 1,
  "packageId": "author.sarah-example",
  "priority": 100,
  "requires": { "plugins": [], "assets": [] },
  "assignments": [
    {
      "target": { "plugin": "Starfield.esm", "localFormId": "00005983" },
      "preset": "Presets/Sarah.npc",
      "scope": "faceAndBody",
      "requires": { "plugins": ["ExampleHairMod.esm"], "assets": [] }
    }
  ]
}
```

## Limits and rules

- Unique `HumanRace` NPC bases only; complete `faceAndBody` replacement only.
  No leveled/template-shared crowds, race or sex conversion, or per-reference
  variation.
- Only the two producers above are supported. CharGenMenu's empty
  `NPCFormEditorID` and quantized tint encoding are part of its verified
  contract; renaming arbitrary JSON to `.npc` does not make it compatible.
- Do not use EditorIDs or load-order-prefixed runtime FormIDs as filenames.
- Do not bundle the OSF Identity DLL in a package — keeping framework, story
  mod, and appearance pack separate stops optional mods from becoming
  story-mod dependencies.

Before publishing, test for every target: a full load, quickload or cell
return, a missing optional dependency, package removal, fallback promotion,
and exact restoration of the original appearance.
