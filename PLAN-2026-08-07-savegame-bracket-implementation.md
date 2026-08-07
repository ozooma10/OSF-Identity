# Plan: implementing the SaveGame-bracket architecture

Prepared: 2026-08-07
Implements: `HANDOFF-2026-08-06-savegame-bracket-architecture.md` (the authoritative design —
this document adds sequencing, file-level structure, and risk management; it does not
redesign). Evidence: notes 15–20 of
`OSF RE/tools/ghidra/context_repo/modules/gameplay.npc_appearance_loader.json`.
Game: Starfield 1.16.244.0 Steam · SFSE Address Library v21.
Line references are against branch `claude/savegame-bracket-architecture-0f11h1` in both repos.

## Context

OSF Identity keeps NPC base records original-at-rest through a scene-lifecycle state machine
(SFSE permanent-task pump, ReferenceSet3d/Detach pending-scene machine, 30-stable-frame gate,
menu deferral, 12 s rollback timer, apply→rebuild→restore latch). The 2026-08-06 OSF RE
evidence proved a simpler invariant — **original-at-serialization** — suffices:

- Silent base mutation does not serialize; the 0x800/0x4000 notifications are both the only
  render-correct path and the changed-form registration.
- Changed-form CONTENT is collected synchronously inside SaveGame 98376 → restoring the base
  at save-entry yields uninstall-benign saves; the render latch keeps it invisible.
- Loading any save reverts bases → re-apply must follow every load.

Target: apply+notify+kick per load, restore-from-owned-snapshot inside the 98376 bracket,
re-apply after save, uninstall = do nothing. Port from the OSF RE prototype
(`OSF RE/src/Probe/NpcAppearanceProbe.cpp` savebake section +
`OSF RE/src/Probe/SaveLoadNameHookProbe.cpp`) — don't reinvent.

**Hard gate: no lifecycle deletion until OQ1–3 are settled by experiment in the OSF RE
savebake harness and milestone 2's fresh-process verdict passes with byte-exact
owned-snapshot restore.** Runtime testing runs through the OSF RE CommandFile channel (this
repo's `RunCommand` has no dispatcher). The user drives in-game steps one instruction at a
time. Test saves: `SAVEBAKE_*` only, deleted afterward; never overwrite real saves. Commits
carry only the user's identity — no AI attribution.

## Why this is a net win (and the honest tradeoffs)

**Simpler**: replaces sampled safety (per-frame pump, 30-stable-frame guess, menu deferral,
detached 12 s rollback thread, generation bookkeeping) with two deterministic engine
chokepoints where the engine announces the exact moments that matter. **More robust**: the
core invariant (saves never carry mod appearance values) becomes measured-synchronous inside
98376 instead of resting on timing heuristics that already produced real failures
(AppHangB1) and on Set3d implicitly re-firing after loads. **More performant**: zero
per-frame cost; work moves to load/save boundaries (~51 ms measured single-target prototype
including preset decode; the production in-memory snapshot restore does less; per-save
timing is logged so multi-target cost is a number, not a hope).

**Tradeoffs accepted**: saves carry a laundered NPC_ changed-form instead of none (docs must
say so honestly — M5); the base is mutated in-session by design; a failed bracket restore
would contaminate that save (hence byte-gated fail-closed everything, byte-exact verify,
explicit exitsave verification); two entry trampolines add plugin-collision surface (gates
fail closed; one hooking DLL per process). New risk classes are gated before commitment:
OQ3 (save-path coverage) is the first experiment and a hard stop; nothing is deleted until
OQ1–3 pass.

## Key exploration findings (verified 2026-08-07)

- **Prototype gaps the port must fix** (`NpcAppearanceProbe.cpp:4782-4823`): the bracket is
  single-target, restores by re-running the full preset pipeline (not byte-exact — leaves
  headpart-list order drift, clobbers faceNPC), is fail-OPEN, and has no post-save re-apply.
- **This repo's gap (largest new-code item)**: `CaptureOwnedVisualSnapshot`
  (`src/NpcAppearance/Runtime.cpp:924-997`, pure read, order-preserving) and the comparator
  `SameExactVisualValues(npc, snapshot)` (`999-1129`, headpart-order-sensitive) exist, but
  there is **no restore-from-snapshot writer** — the only byte-exact writer is `ownedCopy`
  (raw `REL::Offset 0xCD56E0`, gated) from a live donor.
- **The OSF RE harness lacks the owned-snapshot machinery entirely** (grep-verified) → it
  must be ported into the harness so the production restore writer is proven there first.
- This repo has zero SaveGame/LoadGame awareness, zero cosave/serialization (OQ6 answered:
  nothing assumes the old lifecycle's persistence), an unused 4 KB SFSE trampoline
  (`src/main.cpp:38-41`), and no hooking utility — port OSF RE's 90-line
  `src/Util/Hooking.h` (`VerifyExpectedBytes`, `InstallEntryHookWithGateway<N>`).
- `RunCommand` has no dispatcher; ~1900 lines of Runtime.cpp are unreachable diagnostics.
- The 4-line notify+suppress+kick recipe appears 5×; the apply core lives inside the
  515-line `RunTargetTrial` monolith, reached today via synthesized argv
  (`Runtime.cpp:2103-2119`).

---

## Phase 0 — OSF RE harness extensions: settle OQ1–OQ4 (BEFORE any deletion here)

All in `OSF RE` on its designated branch. Findings → module note in
`gameplay.npc_appearance_loader.json`, then `build_starfield_context.py --strict`.

**E1 — Save/Load return callbacks** (`SaveLoadNameHookProbe.cpp` + `NpcAppearanceProbe.h`):
add `OnSaveGameReturn()` / `OnLoadGameReturn()`, called after the gateway returns
(try/catch, never throw across the thunk). Add `SAVE done name=… rc=…` / `LOAD done` lines
so entry/done pairs detect quit-path truncation (log rotation ate them on 2026-08-06).

**E2 — Port owned-snapshot machinery into the harness**: `OwnedVisualSnapshot` +
`CaptureOwnedVisualSnapshot` + `SameExactVisualValues(npc, snapshot)` from this repo's
`Runtime.cpp:882-1129`, plus the NEW production restore writer prototyped there:
`RestoreOwnedVisualSnapshot(out, target, snapshot, originalFaceNPC)` — same gate block as
`SilentApplyPresetToBase` (minus preset/FaceDB work; snapshot AVMs are already
materialized), ONE donor built from the snapshot via the setter family, **verified
`SameExactVisualValues(donor, snapshot)` BEFORE touching the target**, then morphWeight +
setBody loop + skinToneIndex + `ownedCopy(target, donor, false)` + reinstate faceNPC, verify
target vs snapshot, destroy donor, return the verdict.

**E3 — `npcapp savebake bracket2`** (production-shaped bracket, alongside the old verb):
arm = capture owned snapshot pre-mutation → `SilentApplyPresetToBase` → notify 0x800/0x4000
→ kick 101307(actor,false,0x28,false). Save-entry = timed `RestoreOwnedVisualSnapshot`, log
`tid/insideDrain/restoredExact/ms`. Save-return = re-apply only if entry restore verified;
else CRITICAL + skip (dry-run of the OQ5 fail-closed semantics).

**E4 — `npcapp savebake loadprobe`**: on every LoadGame-return, log target resolution +
`hasLoaded3D`, then silent-apply + notify (+ optional kick). Measures OQ1(b) directly.

**Experiment order** (Lane A, one instruction at a time; read `saveload status` counters via
CommandFile before every quit):

1. **P-OQ3 first** (pure logging, no mutation): quicksave, named save, autosave, menu save,
   quit-to-menu, quit-to-desktop → every save type must produce a SAVE entry/done pair
   through 98376. Any bypass ⇒ STOP the whole track and find the second chokepoint.
2. **P-OQ1/OQ2**: `loadprobe on` → load with the target far away, walk to her → verdict
   (preset/original/bald); load with the target nearby → verdict. Then after a correct
   render: interior cell round trip and fast-travel round trip with no re-kick → verdict.
3. **P-OQ4 + M2 gate** (SAVEBAKE_B protocol, new bar): `bracket2 on` → named save +
   quicksave + autosave (thread-policy sampling) → render latch confirmed visually → fresh
   process → baseline at menu → load → **verdict: visualMatch=true vs baseline
   (order-exact), matchesPreset=false, renders original**. Delete the SAVEBAKE_* saves.

**Decision matrix** (drives C3):

- OQ1(b) full pass → load side = apply+notify only; no sink, no suppression; delete both.
- OQ1(b) partial/fail → slim one-shot Set3d sink (base-match + one kick per ref per load
  generation, self-refresh suppressed; no stability frames, no menu logic).
- OQ2 fail → the sink kicks on every Set3d of a target ref.
- OQ4 consistently clean off-drain → the bracket runs off-drain by design; guards = byte
  gates + try/catch + reentrancy latch. Mixed contexts → refuse restore on an unexpected
  context, CRITICAL, per-target skip.
- OQ5 (design, no experiment): per-target try/catch; failed restore ⇒ CRITICAL +
  `bracketFailed=true`, excluded from re-apply until next load; never abort the save.
  Byte-gate mismatch ⇒ global kill switch.

---

## Phase 1+ — this repo (handoff milestones → commits)

**New files**: `src/Util/Hooking.h` (port the whole header from OSF RE);
`src/NpcAppearance/SaveLoadHooks.{h,cpp}` (`SaveLoadNameHookProbe.cpp` ported verbatim minus
probe plumbing: IDs 98376/98380, full 32/31-byte prologue gates, steal 5/7, fail-closed
install; API `bool Install(const Callbacks&)` with
`Callbacks { onSaveGameEntry; onSaveGameReturn; onLoadGameReturn; }`). Add the .cpp to
`xmake.lua` `add_files` (explicit list, lines 20-28). Bracket logic stays inside
Runtime.cpp's anonymous namespace (shares gate tables/typedefs).

**Fail-closed master switch** (Runtime.cpp): `g_bracketOperational` (set only if `Install()`
returned true — a hook gate mismatch disables ALL mutation, not just hooks) and one-way
`g_mutationKilled`. Every mutation entry point checks both.

**C1 = Milestone 1 (hooks)**: Hooking.h + SaveLoadHooks + xmake; `Initialize()` installs
with log-only callbacks. Old lifecycle untouched. Pre-check on the game machine: the
commonlibsf submodule pin provides `REL::GetTrampoline().allocate` / `write_jmp<5>`
(submodules can't be checked from the authoring box). Verify: `tools/verify.ps1` green;
install line `gate=true hook=true` ×2; SAVE-ENTRY/RETURN pair for every save type incl. the
exitsave; LOAD-RETURN per load. **Deploy discipline: never run the OSF RE sandbox DLL in the
same process** — both patch the same entry bytes; the second installer fails its gate closed
by design.

**C2 = Milestone 2 (bracket behind explicit arm)** — one commit, every new symbol
referenced so `-Werror`-class warnings can't strand:

- `SilentApplyPresetToBase(out, target, presetPath)` — typed extraction of the
  `RunTargetTrial` core (gates `3909-3929`, donor pair + registered-empty invariants, copy
  68122, populate, 127/75 preflight, apply, post-validate), signature matching the OSF RE
  prototype (`NpcAppearanceProbe.cpp:3820`) for line-by-line diffability. `RunTargetTrial`
  is NOT rewired yet (the duplication dies in C4b).
- `RestoreOwnedVisualSnapshot` + donor-builder, ported back from the Phase-0-proven harness
  version.
- `NotifyAndKick(...)` — the 4-line recipe; rewire all 5 existing call sites in this commit.
- `AppliedBaseState { baseID, assignment, originalVisual, originalNonVisual,
  originalFaceNPC, originalActorFlags, bracketFailed }` (`PersistentAppliedState` fields
  verbatim), keyed by **baseID** in `g_appliedBases` + mutex + `g_inBracket` reentrancy
  latch.
- `OnSaveGameEntryImpl`: per-target restore + verify, timed; fail ⇒ CRITICAL +
  `bracketFailed=true`. `OnSaveGameReturnImpl`: re-apply per non-failed target.
  `OnLoadGameReturnImpl`: **clear `g_appliedBases` first** (the engine revert pass
  invalidated everything), then per winner: capture, silent-apply, notify (+kick per the
  OQ1 verdict), record; a per-base failure ⇒ no record (never bracket a base we didn't
  mutate).
- Explicit arm for M2: a `bracket.armed` marker file checked in `Initialize()`; without it
  the old scene lifecycle remains the default. The marker dies in C4a.
- Verify (M2 protocol): LOAD-RETURN applied line; `save SAVEBAKE_ID1` →
  `restoredExact=true` + `reapplied=1 failed=0`; render latch visually confirmed;
  fresh-process verdict via the OSF RE sandbox `savebake baseline`/`status`:
  visualMatch=true (order-exact), matchesPreset=false. Delete the test saves.

**C3 = Milestone 3 (load-side recipe)**: implement the OQ1/OQ2 matrix verdict (possibly a
~40-line one-shot Set3d sink). Full-session soak: play, 2+ cell transitions, quicksave,
autosave, named save, one mid-session load, quit. Every SAVE-ENTRY `restoredExact=true`,
zero CRITICAL, fresh-process verdict spot-checked on two session saves (one autosave + the
exitsave).

### C3 result — complete 2026-08-07

The OSF RE `npcapp savebake loadprobe` experiment selected the minimal recipe mechanically:

- With Andreja absent at LoadGame return (`actorMatches=0`, `hasLoaded3D=false`), silent
  base apply plus both notifications rendered the Sarah preset on her first later 3D build.
  No kick was issued.
- With Andreja present at LoadGame return (`actorMatches=1`, `hasLoaded3D=true`), silent
  apply plus both notifications and one immediate 101307 kick rendered the complete preset.
- A loading-door interior round trip and a fast-travel detach/reattach round trip retained
  the complete preset without another kick. Therefore C3 adds no first-Set3d or every-Set3d
  sink.
- Save-return initially exposed one additional rendering requirement: silent reapply alone
  left the loaded actor with the preset face but a missing hair head part after a named save.
  Reloading the clean save exercised the loaded-actor load kick and restored the hair. The
  save-return path now uses the same recipe as load-return: always notify after successful
  base apply, and issue exactly one immediate kick only when the matching actor already has
  loaded 3D. The next named save retained the preset hair.

Production soak with the repaired build:

- Seven save brackets completed: two initial autosaves, one named save, one fast-travel
  autosave, one quicksave, quit-to-main-menu exitsave, and quit-to-desktop exitsave.
- All seven targets logged `restoredExact=true bracketFailed=false`; all seven returns
  logged `reapplied=1 failed=0 insideDrain=true`; three load returns logged
  `applied=1 failed=0 tracked=1 insideDrain=true`; zero CRITICAL lines.
- Visual checks remained `preset hair` after named-save reapply, quicksave reapply,
  fast-travel rebuild, and mid-session load.
- A fresh process with both hook plugins disabled rendered original Andreja from the named
  save, the new quicksave, the fast-travel autosave, and the quit-to-desktop exitsave.
- The protected C2 quicksave remained 2,386,768 bytes and the 208-byte veto placeholder
  remained untouched. No save was deleted.

Required verification passed after the final change: `tools/verify.ps1` and
`tools/build-release.ps1`. The release artifact was
`dist/OSF-Identity-0.1.0.zip` (500,117 bytes, SHA-256
`D8BF141D37D483ED61C60761E50DE289328C5A10331FA7366801717389473737`). The marker gate,
old scene lifecycle, permanent frame pump, target-hold/rollback machinery, and suppression
machinery remain in place for C4; C3 only changes load/save-return rendering behavior.

**C4 = Milestone 4 (swap + delete)**, two commits:

- **C4a swap**: the bracket arms whenever winners exist && operational (marker gate
  removed); `OnNpcAppearanceDataLoaded` stops arming the scene machinery; `main.cpp` drops
  `AddPermanentTask`; `Runtime.h` drops `OnFrame()`. Old machinery becomes unreachable but
  still compiles (still referenced via `RunCommand` — no unused warnings).
- **C4b delete**, whole connected components, grep-audit each symbol before handing the
  commit to the compiling machine:
  1. pump (`RequestNpcAppearanceNativeFrame`) + `OnNpcAppearanceNativeFrame`;
  2. `PendingSceneApply` block + scene counters (keep `g_eventMutex`, `g_targetBaseIDs`,
     `g_sceneAssignments`, the suppression map iff a sink survived C3);
  3. all three sinks + register paths (+ `SuppressNextSceneSet3d` and its `NotifyAndKick`
     line if no sink remains — same commit);
  4. TargetHold component incl. the detached 12 s thread, `RunTargetTrial` latch modes,
     `RunTargetRestore`;
  5. `RunCommand` scope: KEEP pipeline diagnostics (status/selftest/scan/inspect/resolve/
     refs/avm/donor*/copyref — they keep kept helpers referenced and remain the declared
     diagnostic surface), DELETE lifecycle verbs (targethold/targetlatch/targetsnapshot/
     targetrestore/event/scene), rewire `targettrial` to `SilentApplyPresetToBase` +
     `NotifyAndKick`, delete `RunTargetTrial` (kills the argv-synthesis hack at the root),
     add a `bracket` status verb;
  6. uninstall inversion: delete `RemovePersistentAppearances`, `g_persistentAppliedRefs`,
     `ForgetPersistentState`, `PersistentAppliedState`; trim the `RunScan` tail to the
     winners-map writes;
  7. rewrite the `Runtime.h` header comment; update `AGENTS.md` line 3 to the honest new
     threading contract.
  Verify: verify.ps1 green; grep for deleted symbols empty; a soak run behaves identically
  to M3 with no `native-main-thread`/`targethold` log lines.

**C5 = Milestone 5 (docs/tests)**: `docs/PLAYER_GUIDE.md:27` (a save now carries a laundered
NPC_ changed-form holding original values) + AUTHOR_GUIDE only if user-visible behavior
changed; `tools/verify.ps1` + `tools/build-release.ps1` green; `git diff --check` clean.

## Restore-writer decision (recorded rationale)

Donor-mediated restore: all fallible construction happens on a disposable donor verified
against the snapshot **before** the target is touched — no half-restored base can ever
serialize mid-98376; `ownedCopy` is the only proven byte-exact order-preserving writer;
donor lifetime stays inside the call (rule: never retain a donor across a save boundary);
structurally symmetric with the trusted apply path. Headpart append-order and null-vs-empty
container reproduction are proven in Phase 0 by the order-sensitive fresh-process verdict;
contingency if order drifts: remove-all/re-add permutation pass on the donor, still
pre-copy, still fail-closed.

## Risks / watchouts

- The exitsave-on-quit is the contamination vector if the bracket breaks — M1/M3 explicitly
  verify its entry/done pair; CRITICAL logs flush.
- Log rotation: read counters via CommandFile before quitting (harness); SAVE `done` lines
  make truncation detectable (production).
- Never two hooking DLLs in one process (gate collision — fails closed but wastes a launch).
- faceNPC: apply clears it; restore reinstates it after `ownedCopy`; re-apply clears it
  again; the raw pointer never crosses a load (records are rebuilt at every LOAD-RETURN).
- Notify on unloaded actors is unproven for render preparation — don't ship the load recipe
  before the OQ1 measurement.
- Reentrancy: `g_inBracket` latch; a nested SAVE-ENTRY passes through with a CRITICAL log,
  never deadlocks.
- An early autosave after load is safe: records are inserted at LOAD-RETURN before gameplay.

## End state

`tools/verify.ps1` and `tools/build-release.ps1` green; a full-session soak with every save
passing the fresh-process byte-exact verdict; the module note updated in OSF RE (+
`--strict` rebuild); docs honest; the scene-lifecycle state machine gone.
