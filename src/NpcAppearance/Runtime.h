#pragma once

#include <functional>
#include <string>
#include <vector>

// Production runtime for OSF Identity's `.npc` appearance distribution. All
// game-object reads and writes execute from the verified native BSService
// queue's proven drain-owner thread. Startup stays fail-closed when no valid
// package wins or the save/load bracket cannot operate safely.
//
//   npcapp status
//   npcapp selftest
//   npcapp scan [packsRoot]
//   npcapp inspect <preset.npc>
//   npcapp resolve <editorID>
//   npcapp refs <editorID> <preset.npc>
//   npcapp donor [count]
//   npcapp targettrial <editorID> <actorRefID> <preset.npc>
//   npcapp targethold <editorID> <actorRefID> <preset.npc>
//   npcapp targetlatch <editorID> <actorRefID> <preset.npc>
//   npcapp targetsnapshot <editorID> <actorRefID> <preset.npc>
//   npcapp targetrestore
//   npcapp event <status|on|off>
//   npcapp scene <status|on|off|dispatch <on|off>|auto <on|off>|persistent <on [actorRefID]|off>>
//   npcapp copyref <targetRefID> <sourceRefID> [sourceIsPlayer=0|1]
//
// `copyref` is the narrow runtime proof for the vanilla Actor.CopyAppearance
// refresh path (AddressLib ID 97401). It is byte-contract gated and snapshots
// nonvisual base data before/after, but intentionally mutates the target base in
// the current game session. The file importer remains fail-closed until `.npc`
// ownership and decoding are proven.
namespace NpcAppearance
{
    using LineSink = std::function<void(const std::string&)>;

    void Initialize();

    // Retained as an unbound diagnostic surface for focused development builds.
    void RunCommand(const LineSink& a_out, const std::vector<std::string>& a_args);
}
