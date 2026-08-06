# Architecture

How a preset travels from a package folder to a rendered NPC, and where each
stage lives. For the safety contract itself see [AGENTS.md](../AGENTS.md); for
runtime boundaries see [COMPATIBILITY.md](COMPATIBILITY.md).

## Layout

```text
src/
  main.cpp                    SFSE entry: register listener, init, frame task
  NpcAppearance/
    Json.{h,cpp}              strict, bounded JSON reader shared by the parsers
    Config.{h,cpp}            package discovery, manifests, winner selection
    Preset.{h,cpp}            CK / CharGenMenu .npc decoding
    Resolver.{h,cpp}          read-only preset-reference -> game-form resolution
    Runtime.{h,cpp}           scene lifecycle, native-thread apply, diagnostics
  Util/
    NativeMainThreadQueue.*   posting into Starfield's BSService command stream
    StarfieldRuntime.h        memory-range and runtime probing helpers
```

The stages are layered strictly: `Config` and `Preset` are host-pure (they
compile and test on any platform, see `tools/verify-host.sh`), `Resolver` reads
game state but never writes it, and only `Runtime` mutates — and only from the
verified native queue drain.

## Pipeline

1. **Startup** (`Runtime.cpp`, `OnNpcAppearanceDataLoaded`): runs at SFSE
   `kPostPostDataLoad`. No `OSFIdentity/Packages/` directory means the plugin
   stays fully inert.
2. **Discovery and selection** (`Config.cpp`): every package folder becomes a
   manifest — explicit `package.json` or implicit from the
   `Presets/<OwningPlugin>/<LocalFormID>.npc` layout. Validation is
   per-package and per-assignment; one winner per target NPC by priority,
   then lexically smaller `packageId`. Load order never matters.
3. **Decoding** (`Preset.cpp`): strict producer contracts for Creation Kit
   and CharGenMenu `.npc` JSON; any deviation rejects the assignment.
4. **Resolution** (`Resolver.cpp`): every name the preset carries — race,
   head parts, bone groups and sliders, facial shapes, AVM layers, colors —
   must resolve against live game data. Incomplete resolution means no donor
   is ever constructed.
5. **Apply** (`Runtime.cpp`): the SFSE per-frame callback only *requests*
   work; the work itself runs inside Starfield's BSService queue drain, on
   the drain-owner thread, re-verified at execution time. The apply builds a
   temporary donor NPC through the game's own factory, populates it with the
   game's own setters, invokes the vanilla copy-appearance routine onto the
   target, refreshes, destroys the donor, and keeps before/after snapshots so
   the original base stays restorable at rest.

Every native routine involved is located by Address Library ID and gated on
its expected prologue bytes; a mismatch (for example after a game update)
fails the whole apply closed.

## Verification

- `tools/verify.ps1` — full Windows verification: plugin build, both host
  test suites, fixture validator tests, fixture provenance gate.
- `tools/verify-host.sh` — the host-side subset for non-Windows machines.
- `.github/workflows/ci.yml` — runs both of the above on every push: a Linux
  job for fast parser feedback and a Windows job that builds the plugin.
- Host tests prove the parsers only. Game-side lifecycle changes require a
  clean install on the pinned runtime and log/visual evidence — see the
  evidence boundary in [AGENTS.md](../AGENTS.md).
