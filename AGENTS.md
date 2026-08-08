# OSF Identity

- Game-object work runs only from the verified native BSService queue drain.
- The runtime is the render-time overlay (Mechanism B, `docs/OVERLAY_PROBE_FINDINGS.md`): the serializable TESNPC is never preset-mutated at rest — each tracked actor's 3D build gets one drain task that applies, refreshes, and restores the base byte-exactly before returning. Overlay failures render vanilla; a failed in-window restore kills mutation for the process.
- Saves are never blocked: the save/load hooks and their shared C API (v3) are observer-only telemetry, and the engine gateways always run. There is no save bracket, no pre-save restoration, and no persistent mutation tracking.
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
