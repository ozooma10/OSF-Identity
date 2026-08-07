# Author Guide

## Quickstart

An appearance pack replaces the face and body of unique human NPCs with
presets you export from Creation Kit or CharGenMenu. Every target uses the
plugin that owns the NPC plus its plugin-local FormID:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  Sarah Reimagined/               # folder name is the pack ID
    package.json                  # optional priority/shared requirements
    Starfield.esm/
      00005983.npc
      00005983.json               # optional per-preset dependencies
```

To build one:

1. **Export a preset** with Creation Kit `1.16.244` or the verified
   CharGenMenu/SFEE `1.16.244` build, and confirm the producer reloads the
   exact export. Never hand-edit the `.npc`.
2. **Identify the target.** Use the plugin that owns the base NPC plus its
   plugin-local hexadecimal FormID. Put the preset at
   `<OwningPlugin>/<localFormId>.npc`, or use the same tuple in an explicit
   assignment. Never use a load-order-prefixed runtime FormID.
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
discovers `<OwningPlugin>/<localFormId>.npc` files. Add `package.json` only for
a different priority, pack-wide `requires`, or explicit assignments:

```json
{
  "schemaVersion": 1,
  "priority": 100
}
```

`requires` is optional. If omitted, the pack has no declared package-wide
requirements; runtime preset-reference validation still runs before conflict
selection. Declare it only when the pack has plugin or loose-asset
preconditions that cannot be inferred reliably from the preset.

The folder name remains the pack ID even when a manifest is present. Renaming
the folder intentionally changes the ID.

## Manifest reference

- `priority` - `-1000000` to `1000000`, defaulting to `0`. When several valid
  packs target the same NPC, the highest priority wins; ties go to ascending
  case-insensitive pack folder name. Mod-manager order is irrelevant. The
  `100` used in these examples outranks every manifest-less pack.
- `requires` - optional plugins and loose Data-relative assets every preset needs. A
  missing pack-wide requirement disables the whole pack. An explicit
  plugin-local target automatically adds its owning plugin to that assignment.
- If `assignments` is present, only those mappings are used. If it is absent,
  the plugin-folder convention is scanned.

Two folder names that resolve to the same case-insensitive pack ID are both
rejected with `duplicate_package_id`.

`package.schema.json` and `preset-metadata.schema.json` give editor
validation for the files they describe; both files are optional. The runtime
parser is authoritative and rejects unknown properties, duplicate targets, path
traversal, and absolute preset paths.

## Per-preset dependencies

If a single preset needs optional mods, add `<localFormId>.json` beside its
matching `<localFormId>.npc` file inside the owning-plugin directory:

```json
{
  "schemaVersion": 1,
  "requires": {
    "plugins": ["ExampleHairMod.esm"],
    "assets": []
  }
}
```

Sidecar and manifest requirements are additive. `requires` is optional here too;
if omitted, the sidecar adds no requirements. A malformed sidecar disables only
its preset; the rest of the pack and lower-priority candidates still compete.
List every plugin needed to resolve non-vanilla headparts and every loose asset
that needs an availability check.

## Explicit assignments

Packs may map targets to presets directly, with per-assignment `requires`:

```json
{
  "schemaVersion": 1,
  "priority": 100,
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

The target object must contain exactly `plugin` and `localFormId`.
`localFormId` accepts one to eight hexadecimal digits without `0x`, and its
numeric value must not exceed `00FFFFFF`. Equivalent spellings such as `5983`
and `00005983` identify the same target and therefore conflict.

## Limits and rules

- Unique `HumanRace` NPC bases only; complete `faceAndBody` replacement only.
  No leveled/template-shared crowds, race or sex conversion, or per-reference
  variation.
- Only the two producers above are supported. CharGenMenu's empty
  `NPCFormEditorID` and quantized tint encoding are part of its verified
  contract; renaming arbitrary JSON to `.npc` does not make it compatible.
- Plugin-local targets support full, medium, and small plugins and are resolved
  against the current load order. The owning plugin is always an implicit
  dependency, and the local ID must fit that plugin's resolved tier.
- Conflicts are decided after resolution by the actual base FormID. Duplicate
  spellings of one plugin-local tuple are rejected deterministically before
  mutation.
- Do not bundle the OSF Identity DLL in a pack — keeping framework, story
  mod, and appearance pack separate stops optional mods from becoming
  story-mod dependencies.

Before publishing, test for every target: a full load, quickload or cell
return, a missing optional dependency, pack removal, fallback promotion,
and exact restoration of the original appearance.
