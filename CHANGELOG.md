# Changelog

## Unreleased

- Add GitHub Actions CI: a Windows job running the full `tools/verify.ps1`
  (plugin build, host suites, fixture gate) and a Linux job running
  `tools/verify-host.sh`.
- Point the CommonLibSF submodule at its HTTPS URL so `git clone --recursive`
  works without SSH credentials.
- Reorganize sources: `src/Probe/` is now `src/NpcAppearance/` (namespace
  `NpcAppearance`), the shared strict JSON reader lives in
  `NpcAppearance/Json.*`, and `docs/ARCHITECTURE.md` maps the pipeline.
- Reject Windows-rooted preset paths on every platform and add
  `tools/verify-host.sh` so host tests build warning-clean and pass on
  non-Windows machines.
- Rename the install root to `OSFIdentity/Packs/` and flatten convention packs
  to `<packageId>/<OwningPlugin>/<LocalFormID>.npc`; the redundant `Presets/`
  layer is gone.
- Make `package.json` optional. A pack folder holding owning-plugin directories
  is discovered with its lowercased folder name as the `packageId` at priority
  `0`.
- Reject a manifest-less package that has a stray or misnamed root JSON file,
  or a `package.json` nested below the package root, instead of silently
  ignoring it; report loose files dropped directly into `Packs/`.
- Discover packages only in the top level of `Packs/`. A `package.json`
  deeper in the tree is now a diagnosed error rather than a package root.
- Report `discovery=implicit` per pack and `implicitPacks=` in the
  `npcapp scan` summary.

## 0.1.0 - 2026-08-05

- Extract the proven package-driven runtime from OSF RE.
- Support strict Creation Kit 1.16.244 and CharGenMenu `.npc` contracts.
- Add deterministic package conflicts, required-plugin/asset gates, persistent
  scene lifecycle reapplication, exact removal, and release tooling.
- Add byte-exact producer fixture matrices and host verification.
- Add filename-driven `Presets/<OwningPlugin>/<LocalFormID>.npc` community packs
  with optional per-preset dependency sidecars.
- Preserve explicit assignments with additive per-assignment requirements and
  select conflicts only after every candidate passes validation.
