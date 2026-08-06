# Player Guide

## Requirements

- Starfield `1.16.244.0`
- Matching SFSE
- Matching Address Library for SFSE Plugins
- OSF Identity
- At least one compatible appearance package

SFEE/CharGenMenu and the Creation Kit are authoring tools. They are not runtime
requirements for players consuming already-exported presets.

## Install

1. Install the framework archive with your mod manager.
2. Install one or more appearance packages as separate mods.
3. Start the game through SFSE.
4. Load a save containing the targeted NPC.

The framework applies a validated winner after the actor has a stable loaded 3D
generation. Loading screens and the main menu block mutation.

## Conflicts

The package with the highest numeric `priority` wins for an NPC. If priorities
tie, the lexically smaller `packageId` wins. Mod-manager order does not override
that rule.

## Remove or replace a package

1. Save normally, exit Starfield, and disable or remove the package mod.
2. Start the game through SFSE and load the save.
3. The next valid package wins, or the NPC returns to the original appearance.

The framework keeps the base NPC's original values at rest. It does not require
a permanent donor form or write appearance data into your save.

To remove the entire framework, exit the game and disable both the framework and
its appearance packages. A subsequent vanilla actor generation uses the
original base appearance.

## Troubleshooting

Read the newest log at:

```text
Documents\My Games\Starfield\SFSE\Logs\osf-identity.log
```

Common fail-closed messages:

- package directory absent: no appearance package is installed;
- no fully validated winning assignments: every candidate was invalid or lost
  dependency validation;
- required plugin or asset missing: install the author-declared dependency;
- preset rejected: the file is malformed or uses an unsupported producer
  contract;
- runtime contract mismatch: the installed game or Address Library does not
  match the supported version.

Do not rename `.npc` files to work around a rejection. Ask the package author
for a preset exported and reloaded by a supported producer.
