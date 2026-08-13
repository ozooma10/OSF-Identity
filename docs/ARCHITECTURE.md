# Architecture

OSF Identity applies `.npc` presets when actors 3d gets build. The "real" `TESNPC` remains the actor's base and is never mutated. Preset data lives in a detached engine `TESNPC` associated with each tracked NPC.

The complete flow is:

```text
SFSE plugin load
  -> install all read redirects

SFSE kPostPostDataLoad
  -> discover, parse, resolve, and select pack assignments
  -> publish immutable PreparedAssignments to OverlayRuntime

Starfield ReferenceSet3d event
  -> match the actor's TESNPC base to an assignment
  -> if no source exists, retain the reference against that base
  -> queue one preparation job for the base
  -> construct an unregistered FormID-0 TESNPC render source
  -> deep-copy the appearance into independent storage
  -> apply and validate the preset only on the detached render source
  -> publish base -> render-source in an immutable registry
  -> refresh references that built before publication once

Appearance builder and FaceDB work
  -> redirect appearance-data reads to the detached render source
```

## Game integration points

| Integration point | API | Purpose |
| --- | --- | --- |
| Actor 3D lifecycle | `RuntimeComponentDBFactory::ReferenceSet3d` event source | Notifies the runtime whenever the engine assigns 3D to a reference. The event sink ignores non-actors and actors without a selected base assignment. |
| Retry scheduling | `SFSE::TaskInterface::AddPermanentTask` | Rechecks pending bases when the native queue was unavailable during loading. |
| Game-thread execution | `RE::BSService::TaskQueue::QueueTask` | Constructs and publishes each encountered base's render source once, then initiates catch-up refreshes inside the engine's per-frame native task-queue drain. |
| Appearance read redirect | 15 instruction sites | Substitutes a published render source wherever the actor appearance builder or its asynchronous FaceDB graph reads `actor->base`. |

The main appearance APIs are `TESNPC::CopyAppearance`, the headpart/morph/AVM setters, and `Actor::RefreshAppearance`. They operate on the detached render source or actor. No preset API is called on the canonical base.

## Startup preparation

`Config::RunScan` performs all filesystem and data resolution before runtime events are accepted:

1. `PackDiscovery` reads loose files from `Data/SFSE/Plugins/OSFIdentity/Packs/<Pack>/<OwningPlugin>/<LocalFormID>.npc`.
2. `PackScanner` resolves the owning plugin and plugin-local FormID to the loaded runtime FormID. Full, medium, and small plugins are handled separately.
3. The target must resolve to a unique `HumanRace` `TESNPC`.
4. `Preset` parses the CK/CharGenMenu export, and `Resolver` resolves its race, headparts, morphs, colors, and AVM data against loaded game forms.
5. If several valid packs resolve to the same NPC base, the alphabetically earliest pack folder wins.
6. The parsed preset and resolved dependencies are stored in an immutable `PreparedAssignment`, keyed by the target's runtime base FormID.

## Hook installation and failure boundary

`RenderSourceHooks` tries to hook 15 `actor->base` load callsites to redirect to preset TESNPC

Installation is all-or-nothing:

1. Resolve and compare every expected instruction before any executable byte is changed.
2. Generate every trampoline stub and verify that each call displacement is representable.
3. Only after both preflight phases succeed, replace all 15 loads with calls to their stubs.

Any mismatch refuses plugin load. Once installed, an empty registry is behaviorally vanilla: the resolver returns the pointer unchanged.

The generated leaf stubs preserve flags and every scratch general-purpose register they use. The only intentional machine-state change is the destination register of the original load.

## Detached render sources

CommonLibSF's `TESNPC::CreateUnregistered` allocates engine memory and directly invokes the engine `TESNPC` constructor. It does not call the form factory. Because `TESForm` normally assigns and registers a dynamic FormID during construction, the utility brackets the call with the engine's thread-local registration-suppression setter. OSF Identity restricts construction to the native main-thread queue drain and requires the result to have FormID 0 and zero references.

`PrepareRenderSource` then:

1. Copies race, sex/actor flags, skin, height, pronoun, and the engine-owned appearance into the detached object.
2. Requires the copied appearance to match the source exactly and to use independent owned storage.
3. Applies morphs, headparts, colors, and AVM data only to the detached object.
4. Validates the complete prepared result and rechecks the canonical base's state and storage identity.
5. Sets the appearance-rebuild bit only on the detached object.

A failed or duplicate unpublished source is destroyed immediately. 
A published source is intentionally process-lifetime: asynchronous FaceDB nodes can reread the actor base after the initiating task has returned, so reclaiming or replacing a published source would create a use-after-free boundary. 
The immutable registry is bounded to 4096 canonical bases and performs lock-free, allocation-free reads from FaceDB threads.

## Runtime event and queue path

`OverlayRuntime` owns one state record per assigned canonical base:

- `dormant` bases have not yet been encountered.
- The first matching `ReferenceSet3d` moves a base to `pending` and records the reference for catch-up.
- `queued` prevents duplicate preparation while the native task owns the job.
- Successful immutable publication moves the base to `ready`; later 3D builds use the source directly through the hooks.
- Failure before publication moves the base to `disabled`, clears its waiters, and leaves it vanilla.

Additional references encountered while a base is pending or queued join its temporary waiter set. If the native queue is unavailable or drops the guarded task, the base returns to pending. The permanent SFSE task retries pending bases once the queue is usable and becomes an atomic no-op when no preparation work remains.

Publication is irreversible because asynchronous FaceDB work can retain access to the source. After publication, each still-loaded waiting reference receives one `Actor::RefreshAppearance(false, 0x28, false)` catch-up call. A catch-up failure does not falsely disable the published base; future 3D builds can still use its source. 