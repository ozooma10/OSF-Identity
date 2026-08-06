# OSF Identity

An SFSE framework that applies face-and-body appearance presets to unique
Starfield NPCs from drop-in packages, without editing their source plugins.

The framework alone does nothing. Appearance changes are applied only when a
complete, valid package wins selection and passes every validation gate —
anything invalid fails closed and is skipped.

## Requirements

- Starfield `1.16.244.0` with matching SFSE and Address Library
- Presets exported by Creation Kit `1.16.244` or the verified
  CharGenMenu/SFEE `1.16.244` build
- Targets must be unique `HumanRace` NPC bases

Players do not need SFEE installed — CharGenMenu is only used to author
presets.

## Install

Install the release archive with a mod manager, then install appearance
packages as separate mods:

```text
Data/SFSE/Plugins/OSFIdentity/Packages/
  author.package-id/
    package.json          # optional
    Presets/
      Starfield.esm/
        0029A8EB.npc
        0029A8EB.identity.json
```

Removing a package promotes the next valid one, or restores the original
appearance. See the [Player Guide](docs/PLAYER_GUIDE.md).

## How packages are chosen

Presets target an NPC by owning-plugin directory and plugin-local FormID
filename. A package needs no manifest: its folder name, lowercased, becomes the
`packageId` and it runs at the default `priority` of `0`. When several valid
packages target the same NPC, the highest `priority` wins; ties go to the
lexically smaller `packageId`. Mod-manager load order is irrelevant. A
candidate with a missing plugin or asset is skipped — never partially
applied — and the next valid package wins instead.

See the [Author Guide](docs/AUTHOR_GUIDE.md) for the package format and
[Compatibility](docs/COMPATIBILITY.md) for runtime boundaries. Example
packages live in `fixtures/osf-identity/Packages`; they ship no
preset payload, so supply a real producer export.

## Build

```powershell
git clone --recursive <repository-url>   # pulls the pinned CommonLibSF fork
pwsh -NoProfile -File .\tools\verify.ps1
pwsh -NoProfile -File .\tools\build-release.ps1   # writes the ZIP to dist/
```

## Non-goals

No in-game editor, randomizer, procedural crowds, race/sex conversion, or
per-reference variation for a shared base NPC.

## License

MIT
