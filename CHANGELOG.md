# Changelog

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
