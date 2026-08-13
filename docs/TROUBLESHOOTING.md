# Troubleshooting

Start by checking log OSF Identity log:

```text
Documents/My Games/Starfield/SFSE/Logs/OSF Identity.log
```

Packs are scanned once during startup. Fully exit and restart Starfield after installing, removing, renaming, or changing a pack.

## No log file

OSF Identity did not load. Check that:

- Starfield was launched through SFSE.
- `OSF Identity.dll` is enabled and appears at `Data/SFSE/Plugins/OSF Identity.dll` in the mod manager's virtual filesystem.
- Address Library for SFSE is installed and enabled.
- The installed plugin and Address Library support the current Starfield version.

Check the SFSE loader log for a plugin-loading error if `OSF Identity.log` is still absent.

## No valid assignments

Means the plugin loaded, but no preset was accepted:

```text
[PackScanner] startup scan found no valid assignments
```

The preceding warnings explain why. A valid pack uses this exact loose-file
layout:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
  My_Pack/
    Starfield.esm/
      00005983.npc
```

Common discovery messages:

- `package_root_missing` - the `Packs` directory is missing or installed at
  the wrong path.
- `stray_package_root_file` - a file was placed directly inside `Packs/`;
  every pack needs its own folder.
- `invalid_convention_layout` - a preset is at the pack root, a directory is
  nested too deeply, or another unsupported entry was found.
- `invalid_convention_plugin` - the owning-plugin folder is not a valid
  `.esm`, `.esp`, or `.esl` filename.
- `invalid_convention_form_id` - the filename is not a one-to-eight-digit
  plugin-local hexadecimal FormID, contains `0x`, or exceeds `00FFFFFF`.
- `invalid_preset` - the `.npc` or `.json` file is empty, too large,
  inaccessible, or
  resolves outside its pack folder.
- `duplicate_convention_filename` or `duplicate_target` - the pack contains
  more than one filename for the same target.

The pack folders and preset files must be loose files.

## A target or preset was skipped

Look for one of these messages:

- `plugin not loaded` - the owning-plugin folder does not exactly identify an enabled plugin, or that required plugin is missing.
- `formID out of range for plugin tier` - the local FormID does not fit the owning plugin's full, medium, or small tier.
- `did not resolve to TESNPC` - the ID is wrong, refers to a placed actor reference instead of the base NPC, or does not resolve to a supported unique human NPC.
- `preset issue` - the file is malformed or does not satisfy the contract selected by its extension. Use a real Creation Kit/CK-compatible `.npc` or CharGenMenu `.json` export.
- `dependency issue` - a race, headpart, morph, color, or AVM entry referenced by the preset could not be resolved or is incompatible with the target. The issue code and field printed on the same line identify the failed value.
- `shadowed by alphabetically earlier pack` - another valid pack targets the same NPC and won. Pack folder names, not mod-manager priority, decide this conflict.

The preset and target must have the same race and sex. Race conversion, sex conversion, non-human targets, non-unique bases, and per-reference variation are not supported.

## The actor still looks vanilla

Search the log for the actor's base and reference FormIDs. First publication and catch-up end with:

```text
[FaceTextureCompositor] submitted ... generated face-texture requests ...
[OverlayRuntime] face-texture composition completed and render source activated ...
[OverlayRuntime] detached catch-up refresh completed ...
```

Other relevant messages:

- `configured leveled actor mapped` - the placed actor uses a generated runtime NPC rather than the configured pack target. The runtime inherited the configured assignment and will prepare a source for that generated base.
- `runtime FormID reuse detected` - another save reused a generated `FF...` FormID for a different configured NPC. The stale source was deactivated, the affected actor was queued for a vanilla rebuild, and that runtime FormID stays disabled until Starfield restarts.
- `render source ... pending while native queue is unavailable` or `preparation remains pending` - loading temporarily disabled the engine queue. This is expected by itself; the base remains pending and should dispatch when the queue becomes usable.
- `render-source preparation dispatch ... queued` or `ran-inline` - the base-level preparation reached the verified native drain.
- `submitted ... generated face-texture requests` - freckles, wrinkles, moles, complexion, and other post-blend layers reached the engine compositor. The source remains staged and invisible to world appearance reads until those asynchronous resources are ready.
- `face-texture composition completed and render source activated` - the generated resources were finalized into FaceDB before the source became visible, and the waiting actor refresh can consume them.
- `face-texture composition timed out` - the engine did not finish its generated resources within 30 seconds. Appearance injection shuts down instead of publishing an incomplete face.
- `waiting ref ... is no longer loaded or valid` - the actor unloaded before its one-time catch-up refresh. A later 3D build uses the published source directly.
- `dropped by the native queue; retained for retry` - the task was rejected by the drain safety check and will retry. Repeated drops without a later successful application should be reported.
- `rendering vanilla and disabling that base` - detached preparation failed before publication. That NPC remains vanilla for the rest of the session.
- `catch-up refresh failed for published base` - publication already succeeded, so the base is not disabled. A later 3D build can still render from its source.
- `published render-source limit reached` - the session created 2048 distinct sources across configured and generated bases. Further publication fails closed and appearance injection is disabled for the process.
- `appearance injection disabled for the process` - terminal safety shutdown also makes the installed hooks return canonical NPCs instead of published sources.
- `byte gate failed` or `failed closed; refusing to load the plugin` - the installed Starfield executable does not match the supported appearance-read contract. Do not bypass this check; use a matching OSF Identity build.

If the actor only changes after leaving and re-entering the cell, include the
initial-load and re-entry sections of the log in a bug report.

## Native queue drop

This critical message means the guarded preparation payload did not run because it reached the wrong drain thread:

```text
[NativeMainThreadQueue] DROP
```

The rejected base remains pending, so one drop followed by a successful `face-texture composition completed` message is recoverable.
Repeated drops without a later success indicate a queue compatibility problem; preserve the complete log and report it.
Because the rejected payload did not run, this message alone does not indicate that a render source was prepared or published.

## A removed pack still applies

Fully exit Starfield before testing pack removal. Then confirm the old pack is absent from the mod manager's virtual path:

```text
Data/SFSE/Plugins/OSFIdentity/Packs/
```

Also check for duplicate OSF Identity framework installations or another pack targeting the same NPC. 
With no valid pack for that target, a fresh process loads the original appearance without a cleanup command.
