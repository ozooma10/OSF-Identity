# Player Guide

## Requirements

- Starfield `1.16.244.0`
- Address Library for SFSE Plugins
- OSF Identity
- At least one appearance package

## Install

1. Install the framework archive with your mod manager.
2. Install appearance packages as separate mods, plus any hair, beard, or asset dependencies they need.
3. Launch through SFSE and the targetted NPC will have its appearance replaced

## Conflicts

When several valid packages target one NPC, the highest `priority` wins; ties go to the lexically smaller `packageId`. Mod-manager order does not matter. A preset with a missing dependency is skipped so the next valid package can win.

## Removing a package

1. Save, exit Starfield, and disable or remove the package mod.
2. Relaunch through SFSE and load the save.

The next valid package takes over, or the NPC reverts to the original appearance. Packages are scanned once after game data load, so always restart Starfield after adding, changing, or removing one.

The framework keeps the base NPC's original values at rest and never writes appearance data into your save. To remove it entirely, disable the framework and its packages; NPCs revert on their next vanilla regeneration.

## Troubleshooting

Check the newest log:

```text
Documents\My Games\Starfield\SFSE\Logs\osf-identity.log
```

- `package directory absent` - no appearance package is installed
- `no fully validated winning assignments` - every candidate failed validation
- `invalid_package_folder_name` - a package folder needs renaming; ask the
  author, or rename it to letters, digits, `.`, `_`, or `-` only
- `suspect_package_root_file` - a package with no `package.json` has a
  stray or misnamed JSON file at its root, so it was rejected
- `manifest_not_at_package_root` - the package's `package.json` sits in a
  subfolder instead of the package root
- `stray_package_root_file` - a loose file was dropped straight into
  `Packages/` instead of a package folder, and was ignored
- `duplicate_package_id` - two packages claim the same id, so both were
  rejected; rename one folder
- `required package plugin or asset missing` - that package is disabled
- `required preset plugin or asset missing` - only that preset is disabled; a lower-priority candidate may win
- `preset rejected` - malformed file or unsupported producer
- `runtime contract mismatch` - game or Address Library version unsupported
