# Player Guide

## Requirements

- Starfield `1.16.244.0`
- Address Library for SFSE Plugins
- OSF Identity
- At least one appearance pack

## Install

1. Install the framework archive with your mod manager.
2. Install appearance packs as separate mods, plus any hair, beard, or asset dependencies they need.
3. Launch through SFSE and the targeted NPC will have its appearance replaced.

## Conflicts

When several valid packs resolve to one NPC base, the highest `priority` wins;
ties go to the case-insensitively alphabetical pack folder name. Mod-manager
order does not matter. A preset with a missing dependency is skipped so the
next valid pack can win.

## Removing a pack

1. Save, exit Starfield, and disable or remove the pack mod.
2. Relaunch through SFSE and load the save.

The next valid pack takes over, or the NPC reverts to the original appearance. Packs are scanned once after game data load, so always restart Starfield after adding, changing, or removing one.

During play, the NPC's underlying game data keeps its original appearance at all times.
The preset is applied only for the instant the NPC's 3D model is built: OSF Identity
applies it, refreshes the model, and restores the verified original data within the same
engine task. Saving never waits on OSF Identity and is never blocked by it — there is
simply nothing modified for a save to pick up.

The appearance notifications can make Starfield include an `NPC_` changed-form in the
save, but that record contains the verified original values—not the preset. This is what
makes saves uninstall-safe. To remove the framework entirely, exit Starfield, disable OSF
Identity and its packs, then load the save; the NPC uses its original appearance without a
cleanup step.

If anything goes wrong while styling an NPC, that NPC simply renders with its vanilla
appearance for the session.

## Troubleshooting

Check the newest log:

```text
Documents\My Games\Starfield\SFSE\Logs\osf-identity.log
```

- `packs directory is absent` - no appearance pack is installed
- `no fully validated winning assignments` - every candidate failed validation
- `suspect_package_root_file` - a pack with no `package.json` has a
  stray or misnamed JSON file at its root, so it was rejected
- `manifest_not_at_package_root` - the pack's `package.json` sits in a
  subfolder instead of the pack root
- `stray_package_root_file` - a loose file was dropped straight into
  `Packs/` instead of a pack folder, and was ignored
- `duplicate_package_id` - two pack folder names resolve to the same
  case-insensitive ID, so both were rejected; rename one folder
- `required pack plugin or asset missing` - that pack is disabled
- `required preset plugin or asset missing` - only that preset is disabled; a lower-priority candidate may win
- `target plugin absent, tier index invalid, or local FormID exceeds its tier` -
  the canonical plugin-local target is malformed or its owning plugin is not
  loaded
- `package_rejected_duplicate_resolved_target` - two assignments in one pack
  resolve to the same NPC base, so the pack was rejected rather than choosing
  one nondeterministically
- `preset rejected` - malformed file or unsupported producer
- `runtime contract mismatch` - game or Address Library version unsupported
- `mutation killed` - a safety check failed, so OSF Identity stopped changing NPC
  appearance for the rest of the session; affected NPCs render with their vanilla
  appearance and saves are unaffected. Inspect the full log
- `rendering vanilla and disabling this base` - styling that one NPC failed its
  validation; only that NPC falls back to vanilla for the session
