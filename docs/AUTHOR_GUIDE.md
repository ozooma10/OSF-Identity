# Author Guide

## Quickstart

An appearance pack replaces the face and body of unique human NPCs with
presets you export from Creation Kit or CharGenMenu. It is a plain mod folder
holding one preset file per target NPC — no manifest required:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  Sarah Reimagined/               # folder name is the pack ID
    Companion_SarahMorgan.npc     # target NPC's EditorID
    Companion_SarahMorgan.json      # optional per-preset dependencies
```

To build one:

1. **Export a preset** with Creation Kit `1.16.244` or the verified
   CharGenMenu/SFEE `1.16.244` build, and confirm the producer reloads the
   exact export. Never hand-edit the `.npc`.
2. **Name it after the target.** The filename stem must be the target NPC's
   exact EditorID, using 1-128 ASCII letters, digits, or underscores. Presets
   sit directly in the pack folder; nested folders are not scanned.
3. **Name the pack folder.** Its name is the pack ID, exactly as written, and
   can be any folder name accepted by the filesystem. Spaces, capitalization,
   and ordinary punctuation are supported. IDs are compared case-insensitively
   for conflicts and tie-breaking. A stray `package.jsn`, `notes.json`, or a
   `package.json` buried in a subfolder rejects the whole pack rather than being
   ignored.
4. **Declare dependencies.** Optional hair, beard, or asset mods go in a
   sidecar (per preset, below) or in a manifest `requires` (pack-wide).
   Undeclared dependencies are the top cause of "works on my machine" — users
   without them load a broken preset.
5. **Restart Starfield through SFSE.** Packs are scanned once after game
   data load; a quickload will not pick up changes. Confirm your pack ID
   appears as the winner in
   `Documents\My Games\Starfield\SFSE\Logs\osf-identity.log` — failures are
   silent in-game and explained only in the log.

## When you need a manifest

A manifest-less pack runs at priority `0` with no pack-wide requirements. Add
`package.json` at the pack root when you need a different
priority, `requires` shared by every preset, or explicit assignments:

```json
{
  "schemaVersion": 1,
  "priority": 100,
  "requires": { "plugins": [], "assets": [] },
  "presetConvention": "editorIdFilename"
}
```

The folder name remains the pack ID even when a manifest is present. Renaming
the folder intentionally changes the ID.

## Manifest reference

- `priority` - `-1000000` to `1000000`, defaulting to `0`. When several valid
  packs target the same NPC, the highest priority wins; ties go to ascending
  case-insensitive pack folder name. Mod-manager order is irrelevant. The
  `100` used in these examples outranks every manifest-less pack.
- `requires` - plugins and loose Data-relative assets every preset needs. A
  missing pack-wide requirement disables the whole pack.
- Exactly one of `presetConvention: "editorIdFilename"` or
  `assignments`.

Two folder names that resolve to the same case-insensitive pack ID are both
rejected with `duplicate_package_id`.

`package.schema.json` and `preset-metadata.schema.json` give editor
validation for the files they describe; both files are optional. The runtime
parser is authoritative and rejects unknown properties, duplicate targets, path
traversal, and absolute preset paths.

## Per-preset dependencies

If a single preset needs optional mods, add `<EditorID>.json` beside its
matching `<EditorID>.npc` file:

```json
{
  "schemaVersion": 1,
  "requires": {
    "plugins": ["ExampleHairMod.esm"],
    "assets": []
  }
}
```

Sidecar and manifest requirements are additive. A missing or malformed sidecar
disables only its preset; the rest of the pack and lower-priority candidates
still compete. List every plugin needed to resolve non-vanilla headparts and
every loose asset that needs an availability check.

## Explicit assignments

Instead of `presetConvention`, advanced packs may map targets to presets
directly, with per-assignment `requires`:

```json
{
  "schemaVersion": 1,
  "priority": 100,
  "requires": { "plugins": [], "assets": [] },
  "assignments": [
    {
      "target": { "editorId": "Companion_SarahMorgan" },
      "preset": "Companion_SarahMorgan.npc",
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
- Use the exact target EditorID as the filename stem. EditorID matching and
  conflict detection are case-insensitive; case-only duplicate filenames are
  rejected.
- Do not bundle the OSF Identity DLL in a pack — keeping framework, story
  mod, and appearance pack separate stops optional mods from becoming
  story-mod dependencies.

Before publishing, test for every target: a full load, quickload or cell
return, a missing optional dependency, pack removal, fallback promotion,
and exact restoration of the original appearance.
