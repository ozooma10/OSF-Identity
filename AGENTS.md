# OSF Identity

- Game-object work runs only from the verified native BSService queue drain.
- Applied bases may remain mutated in-session; restore every tracked appearance byte-exactly before serialization and veto the save if exact restoration cannot be proven.
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
