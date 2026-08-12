# Architecture

OSF Identity applies `.npc` presets when Starfield builds an actor's 3D. It does not edit the plugin that owns the NPC, and it does not leave the selected appearance on the shared NPC base after the actor has been refreshed.

The complete flow is:

```text
SFSE kPostPostDataLoad
  -> discover, parse, resolve, and select pack assignments
  -> publish immutable PreparedAssignments to OverlayRuntime

Starfield ReferenceSet3d event
  -> match the actor's TESNPC base to an assignment
  -> retain a pending request and enter the native task queue
  -> capture an exact restore donor of the TESNPC base
  -> stage the preset in a temporary TESNPC donor
  -> briefly copy the staged appearance to the shared TESNPC base
  -> destroy the preset donor
  -> refresh the actor's 3D
  -> restore the original TESNPC state and destroy the restore donor
```

## Game integration points


| Integration point | API | Purpose |
| --- | --- | --- |
| Actor 3D lifecycle | `RuntimeComponentDBFactory::ReferenceSet3d` event source | Notifies the runtime whenever the engine assigns 3D to a reference. The event sink ignores non-actors and actors without a selected base assignment. |
| Retry scheduling | `SFSE::TaskInterface::AddPermanentTask` | Rechecks retained requests when the native queue was unavailable during loading. |
| Game-thread execution | `RE::BSService::TaskQueue::QueueTask` | Runs the appearance transaction inside the engine's per-frame native task-queue drain. |

Appearance work main calls are `TESNPC::CopyAppearance`, `TESNPC::CopyOwnedAppearance`, and `Actor::RefreshAppearance`.

## Startup preparation

`Config::RunScan` performs all filesystem and data resolution before runtime
events are accepted:

1. `PackDiscovery` reads loose files from `Data/SFSE/Plugins/OSFIdentity/Packs/<Pack>/<OwningPlugin>/<LocalFormID>.npc`.
2. `PackScanner` resolves the owning plugin and plugin-local FormID to the loaded runtime FormID. Full, medium, and small plugins are handled separately.
3. The target must resolve to a unique `HumanRace` `TESNPC`.
4. `Preset` parses the CK/CharGenMenu export, and `Resolver` resolves its race, headparts, morphs, colors, and AVM data against loaded game forms.
5. If several valid packs resolve to the same NPC base, the alphabetically earliest pack folder wins.
6. The parsed preset and resolved dependencies are stored in an immutable `PreparedAssignment`, keyed by the target's runtime base FormID.

## Runtime event and queue path

The `ReferenceSet3d` sink looks up the prepared assignment by base FormID of the target actor.

`OverlayRuntime` then applies admission control:

- A base disabled after an earlier failure is ignored.
- A reference already in flight is ignored.
- A one-second per-reference cooldown suppresses immediate duplicate rebuilds.
- Accepted work is stored in `m_pendingApplies` before dispatch.

If the native queue is temporarily unavailable, the request remains pending.
The permanent SFSE task retries pending work once the queue is usable.

## Appearance transaction

The appearance change is a short transaction on the shared `TESNPC` base. 

1. `NPCRestorePoint::Capture` records the target's original visual and nonvisual state. It creates a temporary engine `TESNPC` and copies the original appearance into it. This restore donor must own independent visual storage and match the source exactly.
2. `ApplyPreparedAppearance` creates a second temporary `TESNPC`. It copies the target's appearance into this preset donor, applies the prepared preset and resolved forms, and validates the staged result before changing the target.
3. The staged appearance is copied to the target while the original nonvisual state is checked for preservation. The preset donor is then deleted.
4. The target receives change flags `0x800` and `0x4000`, then `Actor::RefreshAppearance(false, 0x28, false)` synchronously rebuilds the actor's 3D from the temporarily modified base. (kind magic number slop from runtime probing)
5. Before the queued task returns, `NPCRestorePoint` copies the original appearance back, restores the explicitly tracked nonvisual fields, and verifies the exact original state.
6. The restore donor is deleted

The actor's rebuilt 3D retains the selected appearance, while the shared base is back in its original state for normal gameplay and serialization. Other references using the same base receive the overlay when their own 3D is built.