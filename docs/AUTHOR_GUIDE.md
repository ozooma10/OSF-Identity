# Author Guide

## Quickstart

An appearance pack replaces the face and body of unique human NPCs with
presets you export from Creation Kit or CharGenMenu. For vanilla NPCs, use an
explicit plugin-local FormID target so the pack does not depend on runtime
EditorID availability:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  Sarah Reimagined/               # folder name is the pack ID
    package.json                  # maps Starfield.esm:00005983 to Sarah.npc
    Sarah.npc
    Sarah.json                    # optional per-preset dependencies
```

To build one:

1. **Export a preset** with Creation Kit `1.16.244` or the verified
   CharGenMenu/SFEE `1.16.244` build, and confirm the producer reloads the
   exact export. Never hand-edit the `.npc`.
2. **Identify the target.** In `package.json`, use the plugin that owns the
   base NPC plus its plugin-local hexadecimal FormID. Zero out the full-plugin
   load-order byte; never use a load-order-prefixed runtime FormID.
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

A manifest-less pack runs at priority `0` with no pack-wide requirements and
uses flat EditorID filenames. That compatibility path works only when the
target EditorID is present in Starfield's runtime lookup table. Add
`package.json` for stable plugin-local targeting, a different priority,
pack-wide `requires`, or explicit assignments:

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
  missing pack-wide requirement disables the whole pack. An explicit
  plugin-local target automatically adds its owning plugin to that assignment.
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

Instead of `presetConvention`, packs may map targets to presets directly, with
per-assignment `requires`. This is the recommended form for vanilla NPCs:

```json
{
  "schemaVersion": 1,
  "priority": 100,
  "requires": { "plugins": [], "assets": [] },
  "assignments": [
    {
      "target": { "plugin": "Starfield.esm", "localFormId": "00005983" },
      "preset": "Sarah.npc",
      "scope": "faceAndBody",
      "requires": { "plugins": ["ExampleHairMod.esm"], "assets": [] }
    }
  ]
}
```

For compatibility with existing packs, an explicit assignment may instead use
`"target": { "editorId": "Companion_SarahMorgan" }`. A target must contain
exactly one locator. EditorID targeting is runtime-dependent and should be
verified for every NPC before publishing.

## Limits and rules

- Unique `HumanRace` NPC bases only; complete `faceAndBody` replacement only.
  No leveled/template-shared crowds, race or sex conversion, or per-reference
  variation.
- Only the two producers above are supported. CharGenMenu's empty
  `NPCFormEditorID` and quantized tint encoding are part of its verified
  contract; renaming arbitrary JSON to `.npc` does not make it compatible.
- Plugin-local targets support full, medium, and small plugins and are resolved
  against the current load order. EditorID matching remains case-insensitive.
- Conflicts are decided after resolution by the actual base FormID, so an
  EditorID target and plugin-local target for the same NPC still compete.
- Do not bundle the OSF Identity DLL in a pack — keeping framework, story
  mod, and appearance pack separate stops optional mods from becoming
  story-mod dependencies.

Before publishing, test for every target: a full load, quickload or cell
return, a missing optional dependency, pack removal, fallback promotion,
and exact restoration of the original appearance.
