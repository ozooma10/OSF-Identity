# Handoff: replace the scene lifecycle with the SaveGame-bracket architecture

Prepared: 2026-08-06
Owning repo: `OSF Identity` (implementation target)
Evidence repo: `OSF RE` (all findings; do not move them, port code)
Game: Starfield 1.16.244.0 Steam · SFSE Address Library v21

## Goal

Replace OSF Identity's runtime lifecycle — the SFSE permanent-task pump, the
`ReferenceSet3d`/`ReferenceDetach` pending-scene state machine, the 30-stable-native-frames
gate, the menu deferral, the rollback timer, and the apply→rebuild→restore latch — with the
**save/load-bracket architecture** proven on 2026-08-06:

- **Per load**: apply the winning preset to the base TESNPC, fire the change notifications,
  kick the refresh — then leave the base mutated for the rest of the session.
- **Per save**: restore the base to its byte-exact original *inside* the engine's SaveGame
  call (before changed-form content collection), let the save write, re-apply after.
  The render latch makes this invisible on screen.
- **Uninstall safety** comes from every save carrying original values — the
  original-at-rest invariant relaxes to **original-at-serialization**, which is what the
  measurements say actually matters.

## Read first, in order

1. Workspace `C:\Modding\Starfield\AGENTS.md` (multi-repo rules — check `git status` per
   repo, never treat `MO2/mods/*` as source, no AI attribution in commits).
2. This repo's `AGENTS.md` and current `src/NpcAppearance/Runtime.cpp` (the architecture
   being replaced; its apply/snapshot/validator primitives are KEPT).
3. `OSF RE/tools/ghidra/context_repo/modules/gameplay.npc_appearance_loader.json` — read
   the **last six notes** (2026-08-06): the savebake matrix, the two-component
   serialization model, and the bracket proof. This is the canonical evidence; do not
   re-derive or contradict it without new runtime proof.
4. `OSF RE/src/Probe/NpcAppearanceProbe.cpp` — the `savebake` section:
   `SilentApplyPresetToBase` (reusable silent-apply core), `RunSaveBakeRefresh`
   (notify+kick variants), the bracket state + `OnSaveGameEntry`.
5. `OSF RE/src/Probe/SaveLoadNameHookProbe.cpp` — the proven SaveGame 98376 / LoadGame
   98380 entry trampolines with full 32/31-byte prologue gates and correct signatures
   (SaveGame name in r9 with `.sfs`; LoadGame name inline at reader+0x000).
6. `OSF RE/Investigations/Requests/2026-08-06-savegame-base-npc-bake-probe.md` and
   `2026-08-06-appearance-apply-hook-seam.md` (how the evidence was produced).

## Proven facts (2026-08-06, all runtime, Sarah 0x00005983 vertical slice)

1. **Silent base mutation does not serialize.** Applying a preset via the donor/lower-worker
   path with no notifications and no refresh leaves saves clean (SAVEBAKE_S).
2. **The `0x800`/`0x4000` base-changed notifications are the coupling point**: they are the
   ONLY render-correct path (they trigger per-headpart preparation; without them a changed
   headpart renders as MISSING — bald — on natural rebuilds, on `101307(0x28)` alone, and
   on the ChangeHeadPart stub recipe `comp+0x582|=4` + `UpdateAppearance(actor,0,0,1)`),
   AND they register the NPC_ changed-form. Registration is at notify time; **content is
   collected at save time, synchronously inside the 98376 call window** (bracket proof,
   SAVEBAKE_B).
3. **Render recipe**: `Notify(0x800)` + `Notify(0x4000)` + `101307(actor, false, 0x28,
   false)` on a loaded actor → full correct render. Sets base actor-flag `0x8000`.
4. **Clearing `0x8000` post-hoc does NOT unregister** the changed-form (SAVEBAKE_C baked);
   it only suppresses the actor-side prepared-data serialization (R rendered afro on load,
   C rendered bald, identical records).
5. **The bracket works**: restore-at-SAVE-ENTRY (51 ms, full 127/75 gates, on the save
   thread, `insideDrain=false` — machinery ran clean off-drain) produced a save that loads
   as vanilla in a fresh process, while the actor visibly kept the preset during and after
   the save (render latch).
6. **Loading any save reverts bases to disk state** (engine revert pass) → re-apply is
   required after EVERY load, not just at startup.
7. **Prepared actor-side data serializes** (SAVEBAKE_R loaded rendering the afro), so one
   notify+kick per load plausibly covers the session — but see Open Question 1.
8. Prototype blemish: restoring by re-applying a CK Baseline preset leaves the headpart
   list order different from pristine (cosmetically identical, renders correctly). The
   production bracket must restore from the **byte-exact plugin-owned snapshot** (this
   repo's `CaptureOwnedVisualSnapshot` / owned-restore path, already proven byte-exact) —
   not by preset re-apply.

## Target architecture

```
data loaded:      scan packs, validate winners, resolve targets   (KEEP, exists)
LoadGame return:  for each winner: capture owned original snapshot,
                  silent-apply preset, notify 0x800+0x4000,
                  kick 101307(actor,false,0x28,false) per loaded target actor
                  [exact firing point: see Open Question 1]
SaveGame entry:   restore each mutated base from its owned snapshot (byte-exact)
SaveGame return:  re-apply preset silently (+ re-notify if OQ2 says needed)
uninstall:        do nothing — every save already carries original values
```

Hooks: port the two entry trampolines from `SaveLoadNameHookProbe.cpp` verbatim —
AddrLib IDs 98376/98380, the full prologue byte gates, steal lengths 5/7, fail-closed
self-disable on gate mismatch (a mismatch must disable ALL mutation, not just the hook).
This repo already allocates a 4KB SFSE trampoline in `main.cpp`.

### Keep from current Runtime.cpp
- Pack/config scan, conflict selection, resolver, CK decoder (unchanged).
- The entire apply stack: donor pair lifecycle, `PopulatePresetMorphs/Visuals`, lower
  owned-copy worker, validators (127/75), byte gates, snapshots,
  `CaptureOwnedVisualSnapshot` + owned restore.
- `NotifyBaseAppearanceChanged`, the 101307 call shape, self-refresh Set3d suppression if
  any sink remains.

### Delete (after the new path passes its gates)
- The SFSE permanent-task pump (`OnFrame`/`RequestNpcAppearanceNativeFrame`) and the
  pending-scene state machine (`g_pendingSceneApply`, stable-frame counting, menu
  deferral, sequence/generation bookkeeping) — unless OQ1 retains a slim Set3d-triggered
  kick, in which case keep only the sink and a one-shot kick, no stability window.
- The targethold/rollback-timer machinery as a runtime mechanism (keep as diagnostics if
  useful).

## Open questions — settle by experiment BEFORE wiring production (the OSF RE savebake
harness answers each in one or two launches; extend it rather than testing in this repo)

1. **Load-side firing point.** The kick needs a loaded actor; at LoadGame-return target
   actors may not have 3D yet, and notify+kick on a not-yet-loaded actor is unproven for
   *render preparation* (notify+refresh on an unloaded actor was proven harmless
   nonvisually in targettrial). Candidate designs: (a) apply+notify at LoadGame-return,
   kick each target on its first `ReferenceSet3d` of the session (slim sink, one-shot, no
   stability window — mutation is only a sanctioned notify+kick now, so the old safety
   scaffolding may be unnecessary); (b) apply+notify+kick all at LoadGame-return and test
   whether actors that build later render correctly (does notify-preparation cover
   actors that load afterwards?). Measure (b) first — if it works the sink dies entirely.
2. **Does the appearance survive a mid-session natural rebuild** (cell round trip /
   detach+reattach) after one notify+kick, without a re-kick? If not, (a) from OQ1 is the
   design.
3. **Do autosave/exitsave/quicksave/console-save all funnel through 98376?** The
   SaveLoadNameHook logging answers this by just playing; log rotation ate the 2026-08-06
   quit-path lines. Any save path that bypasses 98376 breaks the bracket and must be found
   now, not by a user.
4. **Bracket thread policy.** The save thread ran the full donor machinery clean once
   (`insideDrain=false`). Decide: allow off-drain bracket work with guards, or require and
   verify a specific context. Do not silently assume the drain.
5. **Multi-target scaling** (N winners): bracket restores/re-applies must iterate all
   mutated bases; failure on one target must not abort the save or leave a mix — define
   per-target fail-closed semantics (a target that fails restore logs CRITICAL and is
   excluded from re-apply until next load).
6. **Cosave/SFSE serialization**: not needed for the core design (no plugin state must
   survive in the save), but confirm nothing else in this repo assumes the old lifecycle.

## Milestones

1. **Port the hooks** into `src/` (new `Util/SaveLoadHooks` or similar) with gates,
   counters, and a fail-closed master switch; verify install lines in the SFSE log.
2. **Bracket with owned-snapshot restore** for a single target behind an explicit arm;
   re-run the SAVEBAKE_B protocol (mutate+notify+kick → save → fresh process → verdict
   must be: renders original, `matchesPreset=false`, and — new bar — byte-exact headpart
   order vs baseline).
3. **Load-side recipe** per OQ1/OQ2 results; then full-session soak: play, travel, save,
   load, quit; every save from the session must pass the fresh-process verdict.
4. **Swap the startup path**: persistent manager arms the new architecture instead of the
   scene state machine; delete dead machinery; update `AGENTS.md` line 3 (game-object work
   rule) to describe the new threading contract honestly.
5. **Tests/docs**: `xmake build` + `tools/tests` green, `tools/verify.ps1` green; update
   `docs/PLAYER_GUIDE.md`/`AUTHOR_GUIDE.md` only if behavior visible to users changes.

## Rules

- Fail closed everywhere: any gate mismatch (prologue bytes, donor invariants, validator
  failure) disables mutation for the session and logs once, loudly. A failed bracket
  restore must CRITICAL-log and skip re-apply — never let a save proceed silently while
  believing it was laundered when it wasn't.
- Never retain a donor across a save boundary; donors are created and destroyed inside
  each apply/restore call (existing rule, existing behavior of the ported core).
- Runtime testing happens through the OSF RE sandbox CommandFile channel (this repo's
  `RunCommand` has no dispatcher). The user drives in-game steps (saves, loads, visual
  confirmations) — one short instruction at a time; SFSE logs land under
  `<Documents>\My Games\Starfield\SFSE\Logs` (resolve Documents via
  `[Environment]::GetFolderPath('MyDocuments')`).
- Test saves: only NEW named saves (`SAVEBAKE_*` convention), never overwrite real saves,
  delete them when done; remember the game writes Exitsaves on quit.
- Commits are the user's identity only — no AI attribution, no Co-Authored-By trailers.
- New runtime findings go to the OSF RE module note (one canonical home per fact), not
  into this repo's docs.
