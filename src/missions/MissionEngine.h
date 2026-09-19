#pragma once
#include <string>
#include <vector>

// Runs one mission mined by tools/import_missions.py at a time - the payphone assassination
// contracts documented in docs/missions.md. A mission is a small program (MissionData.h) and
// this is its interpreter: an instruction pointer stepping through the donor script's own WAIT/
// IF/GOTO_IF_FALSE/GOTO/variable instructions, with a handler table mapping each opcode a mined
// mission actually uses onto the plugin's existing spawn/blip/message/weapon calls. Nothing here
// touches San Andreas' own script or save state - see docs/missions.md and
// docs/save-compatibility.md for why.
//
// Never active under SA-MP - Install() and StartMission() both refuse the moment
// Settings::RunningUnderSaMp() is true, the same way every other region module does.
namespace MissionEngine {
    // Loads every region's missions.dat (Settings::MapRegion::missionsPath) it can find. Safe to
    // call with no regions configured for missions at all - it then does nothing.
    void Install();

    // Once a frame. Steps whichever mission is running, and checks whether the player has walked
    // into a loaded mission's own ringing-payphone marker (see docs/missions.md).
    void Tick();

    // Starts a mission by id (e.g. "vc_asm1", "iii_mea1" - see docs/missions.md for the full
    // list), at the player's current position. Fails and logs why rather than starting something
    // half-built: no such mission loaded, a mission already running, or under SA-MP. Returns
    // true if it started.
    bool StartMission(const std::string& id);

    // Ends whatever is running, releasing everything it spawned back to the game. Safe to call
    // with nothing running.
    void EndMission();

    // Every mission id currently loaded, for DebugKeys' F-key cycling and the F8 read-out.
    const std::vector<std::string>& LoadedIds();

    // One multi-line block for the F8 read-out: which mission (if any) is running, its
    // instruction pointer, and the last thing it printed.
    void Describe(std::string& out);
    bool Running();     // a mission is in progress (F11 then reports it instead of starting another)
}
