# OSF Identity

- Game-object work runs only from the verified native BSService queue drain.
- Default runtime is the render-time overlay (Mechanism B, `docs/OVERLAY_PROBE_FINDINGS.md`): the serializable TESNPC is never preset-mutated at rest — each tracked actor's 3D build gets one drain task that applies, refreshes, and restores the base byte-exactly before returning. Overlay failures render vanilla.
- The legacy save/load bracket remains armed as backstop (and as the `npcapp overlay off` fallback path): any base it tracks is restored byte-exactly before serialization and the save is vetoed if exact restoration cannot be proven. A failed overlay-window restore escalates the base into bracket custody.
- The mod is unreleased. Prefer the cleanest current schema and runtime contract; do not add backward-compatibility aliases or migrations unless release status changes.

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
