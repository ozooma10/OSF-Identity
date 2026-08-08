#pragma once

// Production runtime for OSF Identity's `.npc` appearance distribution.
// Every game-object read/write runs from the verified native BSService queue
// drain; saves always reach the engine gateway.
//
// The overlay runtime (Mechanism B; see docs/OVERLAY_PROBE_FINDINGS.md)
// styles tracked NPCs per 3D build: one verified drain task applies the
// preset to the base, notifies, refreshes the actor, and restores the base
// byte-exactly before returning — the serializable TESNPC is never
// preset-mutated at rest. Overlay failures render vanilla; a failed
// in-window restore kills mutation for the process.
namespace NpcAppearance
{
    void Initialize() noexcept;
}
