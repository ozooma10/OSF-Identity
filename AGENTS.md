# OSF Identity

- Game-object work runs only from the verified native BSService queue drain.
- Preserve exact original-at-rest values and restore tracked appearances when a package or the framework is removed.

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