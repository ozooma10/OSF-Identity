# Runtime NPC Appearance Distributor

A package-driven SFSE framework that applies complete face-and-body appearance
presets to unique Starfield NPCs without editing their source plugins.

The framework is deliberately fail-closed. Installing only the framework does
nothing: appearance mutation is armed only when at least one complete, valid
package wins discovery and all preset, target, dependency, ownership, thread,
and refresh gates pass.

## Current compatibility

- Starfield `1.16.244.0`
- Matching SFSE and Address Library
- Unique `HumanRace` NPC bases
- Creation Kit `1.16.244` `.npc` exports
- CharGenMenu/SFEE `.npc` exports produced by the verified `1.16.244` build
- Package scope: `faceAndBody`

CharGenMenu is a preset producer only; players do not need SFEE installed to
use a package containing a compatible exported preset.

## Player installation

Install the release archive with a mod manager. Appearance packages are
separate mods and use this layout:

```text
Data/
  SFSE/
    Plugins/
      NpcAppearanceLoader/
        Packages/
          author.package-id/
            package.json
            Presets/
              Appearance.npc
```

Removing a package promotes the next valid winner for that NPC, or restores the
original appearance when no winner remains. See
[Player Guide](docs/PLAYER_GUIDE.md) for the supported removal workflow.

## Package selection

Assignments target a base NPC by plugin name plus plugin-local FormID. Package
load order does not determine the winner: the highest numeric `priority` wins,
then `packageId` ascending breaks ties. Missing plugins, assets, presets, and
invalid assignments are diagnosed and isolated.

The source example is under
`fixtures/npc-appearance-loader/Packages/author.sarah-example`. It intentionally
contains no active preset payload; authors must supply a real producer export.

## Build

Clone recursively so the pinned CommonLibSF fork is present:

```powershell
git clone --recursive <repository-url>
pwsh -NoProfile -File .\tools\verify.ps1
```

Build a release archive with:

```powershell
pwsh -NoProfile -File .\tools\build-release.ps1
```

The ZIP is written to `dist/`. See [Author Guide](docs/AUTHOR_GUIDE.md) and
[Compatibility](docs/COMPATIBILITY.md) for the format and runtime boundaries.

## Proven behavior

The extracted runtime has passed strict package selection, both six-file preset
producer matrices, independently owned donor construction and teardown,
complete morph/headpart/color/AVM materialization, nonvisual preservation,
native-thread lifecycle handoff, loaded-actor refresh, quickload reapplication,
deterministic conflict promotion, package removal, and full framework removal.

The framework does not provide an in-game editor, randomizer, procedural crowd
system, race/sex conversion, or per-reference appearance variation.

## License

MIT
