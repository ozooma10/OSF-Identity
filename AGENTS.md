# OSF Identity

This repository is the production extraction of the Starfield 1.16.244 runtime
NPC appearance work proven in OSF RE.

## Safety contract

- No `NpcAppearanceLoader/Packages` directory means no mutation.
- Invalid packages, missing dependencies, unresolved assets, malformed presets,
  and runtime byte-contract mismatches fail closed independently.
- Game-object work runs only from the verified native BSService queue drain.
- Preserve exact original-at-rest values and restore tracked appearances when a
  package or the framework is removed.
- Only `faceAndBody` on unique HumanRace NPC bases is supported.
- Do not add per-reference variation, race/sex conversion, an editor,
  randomization, or crowd generation without a separate proof program.

## Build and verification

```powershell
pwsh -NoProfile -File .\tools\verify.ps1
pwsh -NoProfile -File .\tools\build-release.ps1
```

Or build directly:

```powershell
xmake f -m debug
xmake build "OSF Identity"
```

Set `XSE_SF_MODS_PATH` to deploy into a matching MO2 mod folder, or
`XSE_SF_GAME_PATH` to deploy into the game Data directory.

## Evidence boundary

Host tests and successful compilation do not prove the game-side lifecycle.
For runtime changes, test a clean install on Starfield 1.16.244 and distinguish
structural log evidence from direct visual confirmation.

The `.npc` files under `fixtures/` are byte-exact producer exports. Never
reformat or normalize them; update their metadata hashes only after an actual
producer round trip.
