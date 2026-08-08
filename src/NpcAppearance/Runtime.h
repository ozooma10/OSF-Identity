#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Production runtime for OSF Identity's `.npc` appearance distribution.
// Validated winners are applied after load and remain in memory during play.
// Every game-object read/write runs from the verified native BSService queue
// drain; tracked bases are restored byte-exactly before serialization and the
// save is vetoed whenever exact restoration cannot be proven.
//
//   npcapp status
//   npcapp bracket
//   npcapp selftest
//   npcapp scan [packsRoot]
//   npcapp inspect <preset.npc>
//   npcapp resolve <plugin:localFormID>
//   npcapp refs <plugin:localFormID> <preset.npc>
//   npcapp donor [count]
//   npcapp targettrial <plugin:localFormID> <actorRefID> <preset.npc>
//   npcapp copyref <targetRefID> <sourceRefID> [sourceIsPlayer=0|1]
//   npcapp overlay [status|on|off|sweep]
//   npcapp probebaseline <plugin:localFormID>
//   npcapp probecompare <plugin:localFormID>
//   npcapp probe97401 <targetRefID> <sourceRefID> [restore=0|1]
//   npcapp probetransient <plugin:localFormID> <actorRefID> <preset.npc>
//   npcapp probeset3d [on|off|status]
//
// `targettrial` only re-applies the bracket-tracked winning assignment and
// issues the selected one-shot notify/refresh recipe; arbitrary or untracked
// mutation is refused. The `probe*` commands are the render-time overlay
// migration's feasibility instruments: they never register state in the save
// bracket, and `probetransient`/`probe97401` restore (or loudly refuse to
// restore) the base within the same drain task.
//
// `overlay on` switches tracked NPCs to the probe-proven Mechanism B runtime
// (docs/OVERLAY_PROBE_FINDINGS.md): per 3D build of a tracked actor, one
// verified drain task applies the preset to the base, notifies, refreshes the
// actor, and restores the base byte-exactly before returning — the
// serializable TESNPC is never preset-mutated at rest. Overlay failures
// render vanilla; a failed in-window restore escalates the base into the
// save bracket's custody and kills mutation.
namespace NpcAppearance
{
    using LineSink = std::function<void(const std::string&)>;

    void Initialize() noexcept;
    void FailClosed(std::string_view a_reason) noexcept;

    // Retained as an unbound diagnostic surface for focused development builds.
    void RunCommand(const LineSink& a_out, const std::vector<std::string>& a_args);
}
