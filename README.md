# OSF Identity

[![CI](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml)

OSF Identity applies appearance presets to starfield NPCs from drop-in packs at runtime without needing to edit any plugins.

## Requirements

- Starfield Steam Edition
- Address Library for SFSE

## Install

Install the release zip with a mod manager.

Then install appearance packs as separate mods:
```text
Data/SFSE/Plugins/OSFIdentity/Packs/   # shared root (packs must be in this folder to be found)
  Sarah_Reimagined/       # arbitrary folder name; this is the pack ID
    package.json          # optional priority/requirements metadata
    Companion_SarahMorgan.npc
    Companion_SarahMorgan.json
```

Removing a pack promotes the next valid one, or restores the original appearance. See the [Player Guide](docs/PLAYER_GUIDE.md).

## How packs are chosen

Presets sit directly in their pack folder and target an NPC by EditorID filename.
Without a manifest, the pack runs at the default `priority` of `0`.

When several valid packs target the same NPC, the highest `priority` wins; ties go to the case-insensitively alphabetical pack folder name. Mod-manager load order is irrelevant.

A candidate with a missing plugin or asset is skipped — never partially
applied — and the next valid pack wins instead.

See the [Author Guide](docs/AUTHOR_GUIDE.md) for the pack format.

Example packs live in `fixtures/osf-identity/Packs`.

## Build

```powershell
git clone --recursive <repository-url>   # pulls commonlibsf submodule
pwsh -NoProfile -File .\tools\verify.ps1
pwsh -NoProfile -File .\tools\build-release.ps1   # writes the ZIP to dist/
```
