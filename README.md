# OSF Identity

[![CI](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/OSF-Identity/actions/workflows/ci.yml)

OSF Identity distributes appearance presets to starfield NPCs from drop-in packs at runtime without needing to edit any plugins or baking the presets into the save.

As such it is safe to add/remove/change presets mid-save.

## Requirements

- Starfield Steam Edition
- Address Library for SFSE

## Install

Install the release zip with MO2/Vortex.

Then install appearance packs as separate mods:
```text
Data/SFSE/Plugins/OSFIdentity/Packs/   # shared root (packs must be in this folder to be found)
  Sarah_Reimagined/                 # arbitrary folder name; this is the pack ID
    Starfield.esm/                  # plugin that owns the target NPC
      00005983.npc                  # plugin-local FormID
```

Removing a pack promotes the next valid one, or restores the original appearance. For issues, refer to [TroubleShooting](docs/TROUBLESHOOTING.md).

## How packs are chosen

Every target is identified by its owning plugin plus plugin-local FormID; 
Packs discover presets from `<OwningPlugin>/<localFormId>.npc`.

Runtime/load-order FormIDs and EditorID targets are not accepted.

When several valid packs resolve to the same NPC base, the alphabetically earliest pack
folder name wins

Mod-manager load order is irrelevant.

A candidate with a missing plugin or asset is skipped and the next valid pack wins instead.

See the [Author Guide](docs/AUTHOR_GUIDE.md) for the pack format.

Example packs live in `fixtures/osf-identity/Packs`.

## Build

```powershell
git clone --recursive <repository-url>   # pulls commonlibsf submodule
xmake build
```
