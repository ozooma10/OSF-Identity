# Compatibility and Safety Boundaries

## Runtime pin

Version `0.1.0` is proven only on Starfield `1.16.244.0` with the matching SFSE
and Address Library. Native helpers and data access are guarded by Address
Library IDs, byte contracts, runtime memory checks, and thread ownership checks.
An unsupported runtime must fail closed; it is not assumed compatible because
the plugin happens to load.

## Preset producers

Creation Kit and CharGenMenu use the same broad JSON shape but do not encode all
fields identically. The framework has independent round-tripped six-file fixture
matrices for both producers.

CharGenMenu compatibility does not create an SFEE runtime dependency. SFEE is
only needed by authors producing or editing CharGenMenu presets.

## NPC and mod compatibility

Assignments resolve base NPCs by owning plugin plus local FormID, so normal load
order changes do not retarget them. Full, medium, and light plugins are supported.
The target must resolve to a unique HumanRace NPC.

Packs can declare required plugins and Data assets. A missing dependency
disables only the invalid pack or assignment; other valid winners continue.

Two appearance packs targeting the same NPC use deterministic priority and
package-ID selection. The framework does not merge partial appearances.

## Save behavior

Applied rendered values are owned independently while the original TESNPC base
remains restored at rest. Actor refreshes, detach/attach generations, cell
returns, and quickloads are handled through the verified scene lifecycle. No
temporary donor form remains registered after application.

## Explicit non-goals

- generic or procedural crowd distribution;
- per-reference variation for a shared base NPC;
- race or sex transformation;
- an in-game appearance editor;
- randomization;
- partial face-only, body-only, or tint-only pack scopes;
- compatibility claims for untested game versions.
