# Appearance Pack Author Guide

OSF Identity uses a filesystem-only pack format. A pack is just a folder of
`.npc` presets arranged by target plugin and plugin-local FormID.

## Pack layout

Install packs beneath:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
```

A complete pack looks like this:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  Cool_Appearance_Pack/              # pack name
    Starfield.esm/                  # plugin that owns the target NPC
      00005983.npc                  # target's plugin-local FormID
    ExampleCompanion.esm/
      0000082A.npc
```

All pack folders and `.npc` files must be installed as loose files. OSF Identity does not discover packs from BA2 archives.

## Creating a pack

1. Export a `.npc` preset with the Creation Kit or from chargenmenu.
2. Find the target's base NPC record. Use the plugin that owns that record and  plugin-local FormID.
3. Create a directory named exactly like the owning plugin, including its `.esm`, `.esp`, or `.esl` extension.
4. Name the preset with the hexadecimal local FormID and a `.npc` extension.
5. Put the plugin directory inside a uniquely named pack folder under `Packs/`.
6. Fully restart Starfield through SFSE. Packs are scanned once during startup.

For example, Sarah Morgan's base NPC is `Starfield.esm:00005983`, so her preset
belongs at:

```text
Cool_Appearance_Pack/Starfield.esm/00005983.npc
```

## Target rules

- FormIDs must contain one to eight hexadecimal digits without `0x`.
- The numeric local FormID cannot exceed `00FFFFFF`.
- Full, medium, and small plugins are supported.
- Targets must resolve to unique `HumanRace` NPC bases.
- The preset must use the same race and sex as the target. Race and sex conversion are not supported.
- Each pack may contain only one preset for a target. `5983.npc` and `00005983.npc` identify the same local FormID and therefore conflict.
- A preset replaces the target's complete face-and-body appearance. OSF Identity does not apply independent appearance layers.

The runtime validates forms referenced by the preset, including its race, headparts, morph data, colors, and AVM entries. 
If required forms cannot be resolved, that preset is skipped.

## Conflicts

If several valid packs target the same NPC, the alphabetically earliest pack
folder name wins. Comparison is case-insensitive first and does not use
mod-manager priority.

Choose a stable, distinctive pack folder name. The log identifies both the
winner and any pack it shadows.

## Runtime and saves

OSF Identity applies the selected preset when the actor's 3D is built, refreshes that actor, and restores the original NPC base before the native task ends.
The preset is therefore visible in game without being left on the NPC base for a save to serialize.

Removing a pack requires a full game restart. With the pack absent, another valid pack can win; otherwise the NPC loads with its original appearance.

## Logs

```text
Documents/My Games/Starfield/SFSE/Logs/OSF Identity.log
```
