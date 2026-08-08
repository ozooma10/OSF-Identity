# Overlay Feasibility Probe Findings — 2026-08-07

> Historical record. The `npcapp` probe/diagnostic commands and the `idnpcapp` bridge described
> here were deleted from the runtime after these findings were locked in (2026-08-08); this
> document is the record of what they proved.

In-game probe session for the render-time overlay migration (plan: mutate→restore→veto bracket
replaced by an overlay whose invariant is "the serializable TESNPC is never preset-mutated at
rest; every failure renders vanilla; saves always reach the engine gateway"). Game 1.16.244,
probes driven through the OSF RE sandbox CommandFile channel via the `idnpcapp` bridge.

Test subject: Andreja — base `Starfield.esm:000059A7`, ref `000059A9` (`AndrejaREF`), preset
`codex.c3-andreja-smoke\Sarah.npc` (validated winner, priority 100). Session state: bracket
armed, `appliedBases=0` throughout (probes never register bracket state).

## (a) Does engine worker 97401 write the target base? — YES (Mechanism A dead)

`probe97401 000059A9 14 1` (source = player, restore-after):

```
RESULT baseVisualMutated=true baseNonVisualMutated=true baseNonVisualMutatedIgnoringDirtyFlag=false
       faceNPCChanged=true flagsChanged=true sourceVisualUntouched=true ms=0.012
visual diff: morph=false skinTone=false headParts=false boneValues=false boneRegions=false
             avms=false shapeBlends=false teeth=false facial=false  (bodyRegions/jewelry/eye/hair/eyebrow unchanged=false means changed)
restored exact=true
```

The worker copies the source's appearance into the **target's TESNPC base** (morphs, head parts,
bone data, AVMs, shape blends, skin tone, colors, faceNPC). It is not an actor-level overlay.
The only non-visual delta was the refresh-dirty flag. Restore was byte-exact.

## (b) Does the 97401 effect serialize? — NOT TESTED, MOOT

Only relevant to Mechanism A. Under Mechanism B mutation never exists at rest, so there is
nothing for a save to catch. Skipped deliberately.

## (c) Does a single-drain transient window render? — YES (Mechanism B CONFIRMED)

`probetransient Starfield.esm:000059A7 000059A9 ...\Sarah.npc` with Andreja's 3D loaded:

```
silentapply: applied=true ... matchesPreset=true nonVisualPreserved=true donorsUnregistered=true
ownedrestore: donorExact=true targetExact=true donorUnregistered=true
window CLOSED applied=true notifiedKicked=true restoredExact=true ms=48.682
RECHECK (+2s) baseStillExact=true
```

**Visual verdict (operator): Andreja renders the Sarah preset** — and keeps it — while the base
is provably byte-exact original before the drain task even returns. The appearance build
consumes the mutated base synchronously inside the 101307 refresh: the probe's own
`ReferenceDetach` + re-`ReferenceSet3d` pair for her ref (6 ms apart, inside the 48.7 ms window)
shows refresh = teardown + rebuild of the actor's 3D.

Post-session `probecompare`: `visualExact=true faceNPCSame=true`, but actor-base flag bit
`0x00400000` differed from the pre-probe baseline (`0x404000B3` vs `0x400000B3`). The +2 s
RECHECK passed full-flags equality, so this bit was set later by normal gameplay (live AI
actor), not leaked by the window. Note for Phase 2: window must keep capturing/restoring
`actorBaseFlags` exactly as the probe does; flags moving *outside* the window are the engine's
business.

## (d) Is ReferenceSet3d a usable trigger? — YES (with mandatory filtering + post)

- `SET3D` fires **per actor, at 3D build time**: four Constellation members fired individually
  as the operator walked through the Lodge; Andreja fired `tracked=true` the moment she
  streamed in. `DETACH` fires on teardown (and during 101307 refresh).
- Events fire **outside the drain** on varying threads (`insideDrain=false`, tids 21944/24472/
  32116 observed) → the Phase 2 sink must post, never apply inline. Measured event→drain
  latency: **1.76–1.98 ms** — visually instant.
- Volume: 7500 set3d / 7314 detach events in ~8 min, of which only **16** were actors and 3
  tracked. The ref→Actor→tracked-base filter in the handler is mandatory and sufficient.

## Decision

Per the migration plan's table: (a)=yes, (c)=yes → **Mechanism B: transient-window
apply→notify(0x800,0x4000)→refresh(101307)→restore per 3D build**, triggered by a posted
`ReferenceSet3d` sink (plus a load-completion sweep for actors already 3D at arm time).
Phase 2 proceeds on this basis; the bracket stays armed as backstop until the overlay soaks.

Residual obligation under B: the in-window restore must succeed (failure ⇒ KillMutation while
the bracket still exists; after Phase 4 it is the one remaining hard-fail path). The window is
same-thread with saves (drain task), so a save can never interleave a mutated base.
