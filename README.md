# OSF Identity

[![CI](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml)

An SFSE framework that applies face-and-body appearance presets to unique
Starfield NPCs from drop-in packs, without editing their source plugins.

The framework alone does nothing. Appearance changes are applied only when a
complete, valid pack wins selection and passes every validation gate —
anything invalid fails closed and is skipped.

## Requirements

- Starfield `1.16.244.0` with matching SFSE and Address Library
- Presets exported by Creation Kit `1.16.244` or the verified
  CharGenMenu/SFEE `1.16.244` build
- Targets must be unique `HumanRace` NPC bases

Players do not need SFEE installed — CharGenMenu is only used to author
presets.

## Install

Install the release archive with a mod manager, then install appearance packs
as separate mods:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  author.package-id/
    package.json          # optional
    Starfield.esm/
      0029A8EB.npc
      0029A8EB.identity.json
```

Removing a pack promotes the next valid one, or restores the original
appearance. See the [Player Guide](docs/PLAYER_GUIDE.md).

## How packs are chosen

Presets target an NPC by owning-plugin directory and plugin-local FormID
filename. A pack needs no manifest: its folder name, lowercased, becomes the
`packageId` and it runs at the default `priority` of `0`. When several valid
packs target the same NPC, the highest `priority` wins; ties go to the
lexically smaller `packageId`. Mod-manager load order is irrelevant. A
candidate with a missing plugin or asset is skipped — never partially
applied — and the next valid pack wins instead.

See the [Author Guide](docs/AUTHOR_GUIDE.md) for the pack format and
[Compatibility](docs/COMPATIBILITY.md) for runtime boundaries. Example
packs live in `fixtures/osf-identity/Packs`; they ship no
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
