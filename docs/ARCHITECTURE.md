# Architecture

OSF Identity applies `.npc` and `.json` presets when actor 3D is built. The "real" `TESNPC` remains the actor's base and is never mutated. 
Preset data lives in a detached engine `TESNPC` associated with each tracked NPC.

The complete flow is:

```text
SFSE plugin load
  -> install all read redirects

SFSE kPostPostDataLoad
  -> discover, parse, resolve, and select pack assignments
  -> register the ReferenceSet3d sink
  -> publish immutable PreparedAssignments to OverlayRuntime

Starfield ReferenceSet3d event
  -> match the actor's TESNPC base directly, or recover its configured base from leveled-reference extra data
  -> inherit the configured assignment into a per-generated-base runtime state
  -> if no source exists, retain the reference against that base
  -> queue one preparation job for the base
  -> construct an unregistered FormID-0 TESNPC render source
  -> deep-copy the appearance into independent storage
  -> apply and validate the preset only on the detached render source
  -> stage base -> render-source in the lock-free registry
  -> submit its AVM layers to the engine face-texture compositor
  -> wait for the generated texture resources, finalize them into FaceDB, advance one engine-equivalent state tick, and wait for the engine publication barrier
  -> activate the render source for world reads
  -> refresh references that built before activation once

Appearance builder and FaceDB work
  -> redirect appearance-data reads to the active detached render source
  -> use the configured plugin-owned base only for generated face-texture naming identity
```

## Game integration points

| Integration point | API | Purpose |
| --- | --- | --- |
| Actor 3D lifecycle | `RuntimeComponentDBFactory::ReferenceSet3d` event source | Notifies the runtime whenever the engine assigns 3D to a reference. Direct bases match assignments immediately; leveled/template-generated actors recover their configured base through the same engine helper used by `ObjectReference.GetBaseObject`. |
| Retry scheduling | `SFSE::TaskInterface::AddPermanentTask` | Rechecks pending bases when the native queue was unavailable during loading. |
| Game-thread execution | `RE::BSService::TaskQueue::QueueTask` | Constructs and stages each encountered base's render source once, activates it after any generated textures are ready, then initiates catch-up refreshes inside the engine's per-frame native task-queue drain. |
| Appearance read redirect | 20 instruction sites | Fifteen sites substitute an active render source wherever the appearance builder or asynchronous FaceDB graph reads `actor->base`. A staged source remains invisible to these reads. Of the five texture-identity sites, the preliminary FaceDB selection read uses the current runtime base; the owner and masked-FormID naming reads use the configured plugin-owned base. This keeps generated leveled actors on the correct FaceDB branch while giving their complexion textures the stable `Starfield.esm:<configured FormID>` key. |
| Post-blend texture generation | Engine CharGen face compositor | Submits the detached source's complete AVM layer set while it is staged, waits asynchronously for all generated texture resources, finalizes them into FaceDB, and only then activates the source and refreshes waiting actors. |

The main appearance APIs are `TESNPC::CopyAppearance`, the headpart/morph/AVM setters, and `Actor::RefreshAppearance`. They operate on the detached render source or actor. No preset API is called on the canonical base.

## Startup preparation

`Config::RunScan` performs all filesystem and data resolution before runtime events are accepted:

1. `PackDiscovery` reads loose `.npc` and `.json` files from `Data/SFSE/Plugins/OSFIdentity/Packs/<Pack>/<OwningPlugin>/<LocalFormID>.<format>`.
2. `PackScanner` resolves the owning plugin and plugin-local FormID to the loaded runtime FormID. Full, medium, and small plugins are handled separately.
3. The target must resolve to a unique `HumanRace` `TESNPC`.
4. `Preset` dispatches by extension and normalizes either the CK-compatible NPC contract or CharGenMenu Version 2 JSON contract. 
   `Resolver` resolves EditorID references from `.npc` files and plugin-local form references from `.json` files, then prepares headparts, morphs, colors, and AVM data.
5. If several valid packs resolve to the same NPC base, the alphabetically earliest pack folder wins.
6. The parsed preset and resolved dependencies are stored in an immutable `PreparedAssignment`, keyed by the configured target's runtime FormID.
7. `OverlayRuntime::Arm` accepts the immutable configured assignment set. The render-source registry separately enforces its publication limit as canonical and generated bases are encountered.

## Hook installation and failure boundary

`RenderSourceHooks` hooks 15 `actor->base` load sites plus five FaceDB texture-identity sites.

Installation is all-or-nothing:

1. Resolve and compare all 20 expected instruction sequences before any executable byte is changed.
2. Generate every trampoline stub and verify that each call displacement is representable.
3. Only after both preflight phases succeed, replace all 15 loads with calls to their stubs.

Any mismatch refuses plugin load. Once installed, an empty registry is behaviorally vanilla: the resolver returns the pointer unchanged.

The generated leaf stubs preserve flags and every scratch general-purpose register they use. The only intentional machine-state change is the destination register of the original load.

## Detached render sources

CommonLibSF's `TESNPC::CreateUnregistered` allocates engine memory and directly invokes the engine `TESNPC` constructor. It does not call the form factory. Because `TESForm` normally assigns and registers a dynamic FormID during construction, the utility brackets the call with the engine's thread-local registration-suppression setter. OSF Identity restricts construction to the native main-thread queue drain and requires the result to have FormID 0 and zero references.

`PrepareRenderSource` then:

1. Copies race, sex/actor flags, skin, height, pronoun, and the engine-owned appearance into the detached object.
2. Requires the copied appearance to match the source exactly and to use independent owned storage.
3. Clears inherited facial-morph values from the detached copy, supplies the configured race when a generated leveled base did not materialize its inherited race, then applies the complete preset's morphs, headparts, colors, and AVM data only to that detached object. Body-region values go through `TESNPC::SetBodyMorph`, which owns the five-element allocation/null contract. A generated base with a real non-null race mismatch is still rejected.
4. Validates the complete prepared result and rechecks the canonical base's state and storage identity.
5. Sets the detached object's source-local rebuild state: the actor rebuild bit plus NPC appearance-change bits `0x800 | 0x4000`. It does not call `TESNPC::AddChange`, enter the engine change manager, or dirty the canonical form.

The source remains unregistered with FormID 0 for its full lifetime. FaceDB nevertheless requires a registered identity to name and cache generated complexion textures; using the source there resolves customized faces to `00000000_*.dds` or an `UnknownOwnerFile` compositor key. A generated leveled base is also unsuitable because its ownerless dynamic FormID changes between runs. Five byte-gated texture-identity thunks therefore keep the preliminary FaceDB selection tied to the current runtime base but use the configured plugin-owned NPC for final owner and FormID naming reads. Headparts, morphs, AVM layers, and colors continue to come from the FormID-0 source, so texture naming cannot leak configured appearance data back into the render path.

Normal actor rebuilds only request a pre-generated face DDS; they do not composite `TESNPC::tintAVMData`. For sources with post-blend layers, `FaceTextureCompositor` therefore invokes the same engine compositor used by CharGen. Its 0x48-byte output carrier is constructed by the engine, retained for process lifetime after submission, and polled only inside the native main-thread drain. `OverlayRuntime` acquires one process-wide preparation owner before queueing any base and retains it through compositor submission, readiness polling, finalization, the publication barrier, and activation; other bases remain pending until that owner activates or fails cleanly before publication. This serializes access to the engine's shared face-customization render resources rather than merely serializing the brief submission calls. After every asynchronous texture handle reports ready, the runtime finalizes the output, advances through the same separate post-finalization tick used by CharGen, then polls CharGen's engine-owned publication-busy byte before activation. The compositor consumes all AVM inputs from the detached source while its final cache-key reads use the configured plugin-owned NPC. The forward `actor->base` redirect remains inactive until the publication barrier clears so the world renderer cannot request and cache the generated face path before FaceDB has published it.

A failed or duplicate unpublished source is destroyed immediately.
A staged or active source is intentionally process-lifetime: compositor and asynchronous FaceDB nodes can retain it after the initiating task has returned, so reclaiming or replacing it would create a use-after-free boundary.
The registry has 4096 slots and admits at most 2048 published sources, keeping its open-addressed tables at or below a 50% load factor. The publication count and both append-only table insertions are serialized by the publication mutex; duplicate publication does not consume capacity. The primary table is keyed by the canonical base's nonzero runtime FormID; a secondary index maps the detached source pointer back to that primary slot for average O(1) compositor and FaceDB reverse lookups. A primary slot normally moves from staged to active. Save loading can replace an ownerless generated `TESNPC` pointer while preserving its runtime FormID; when the configured base identity also matches, the replacement resolves the same active source without a registry mutation. `OverlayRuntime` retains that configured FormID after publication and validates it on every `ReferenceSet3d`. If another save reuses the dynamic FormID for a different configured NPC, the stale slot is atomically deactivated, the affected loaded reference is rebuilt against vanilla data in the native queue drain, and that runtime FormID remains disabled until Starfield restarts. Reads from FaceDB threads remain lock-free and allocation-free.

## Runtime event and queue path

`OverlayRuntime` owns one state record per assigned canonical base:

- `dormant` bases have not yet been encountered.
- When an actor has a generated `TESNPC` in `actor->base`, the runtime resolves the configured base retained by `ExtraLeveledCreature`. If that configured base has an assignment, the generated FormID receives its own state with the same immutable assignment. Preparation and publication use the generated base so the appearance hooks replace the exact pointer read by the engine.
- The first matching `ReferenceSet3d` moves a base to `pending` and records the reference for catch-up.
- `queued` prevents duplicate preparation while the native task owns the job. A single process-wide owner covers the complete preparation transaction, so another base cannot enter `queued` while the owner is composing or waiting for publication.
- `composite pending`, `composite queued`, `composite finalized`, and `composite activation queued` retain references while an AVM-bearing source is staged, its generated face textures are in flight, and FaceDB publication is settling behind the engine barrier.
- Successful texture finalization activates the source and moves the base to `ready`; later 3D builds use the source directly through the hooks.
- A post-load 3D rebuild may use a replacement `TESNPC` pointer for the same dynamic FormID. When its configured base identity is unchanged, the normal appearance hooks find the active source during the rebuild without rerunning the preset or compositor. A different configured identity deactivates the stale binding and queues a vanilla rebuild instead.
- Failure before publication moves the base to `disabled`, clears its waiters, and leaves it vanilla.

Additional references encountered while a base is pending or queued join its temporary waiter set. If the native queue is unavailable or drops the guarded task, the base returns to pending. The permanent SFSE task retries pending bases once the queue is usable and becomes an atomic no-op while no base is dispatchable, including while the native queue already owns the only outstanding job.

Staging is irreversible because compositor and asynchronous FaceDB work can retain access to the source. Activation is a separate publication step. The runtime therefore requires the owning base to make the exact `queued -> composite pending -> composite queued -> ready` transition for AVM-bearing sources, or `queued -> ready` when no post-blend layers exist, with the same prepared assignment. An impossible transition disables appearance injection process-wide. A composite that does not finish within 30 seconds also triggers terminal fail-closed shutdown without exposing its source to world reads. After activation, each still-loaded waiting reference receives one `Actor::RefreshAppearance(false, 0x28, false)` catch-up call. A catch-up failure does not falsely disable the active base; future 3D builds can still use its source.
