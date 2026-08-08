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
//   npcapp overlay [status|on|off]
//   npcapp probebaseline <plugin:localFormID>
//   npcapp probecompare <plugin:localFormID>
//   npcapp probe97401 <targetRefID> <sourceRefID> [restore=0|1]
//   npcapp probetransient <plugin:localFormID> <actorRefID> <preset.npc>
//   npcapp probeset3d [on|off|status]
//
// `targettrial` only re-applies the bracket-tracked winning assignment and
// issues the selected one-shot notify/refresh recipe; arbitrary or untracked
// mutation is refused. The `overlay`/`probe*` commands are the render-time
// overlay migration's feasibility instruments: they never register state in
// the save bracket, and `probetransient`/`probe97401` restore (or loudly
// refuse to restore) the base within the same drain task.
namespace NpcAppearance
{
    using LineSink = std::function<void(const std::string&)>;

    void Initialize() noexcept;
    void FailClosed(std::string_view a_reason) noexcept;

    // Retained as an unbound diagnostic surface for focused development builds.
    void RunCommand(const LineSink& a_out, const std::vector<std::string>& a_args);
}
