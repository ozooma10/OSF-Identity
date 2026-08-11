# OSF Identity

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
