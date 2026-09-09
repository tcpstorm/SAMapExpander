// Asking the streamer for a model without keeping it for ever.
//
// THE BUG THIS EXISTS TO FIX, and it reached Los Santos.
//
// The pedestrians asked for a district's ped model with
// CStreaming::RequestModel(id, GAME_REQUIRED) and left it at that, on the reasonable-sounding
// assumption - it was written into the comment - that the streamer is then "free to throw it out
// again when the player walks away". It is not, and the disassembly is unambiguous about it:
//
//   CStreaming::RemoveLeastUsedModel (0x40CFD0) walks the loaded list and at 0x40D029 does
//   `test byte ptr [edi+6], bl` against the caller's don't-remove mask, skipping any entry whose
//   flags match. GAME_REQUIRED (0x02) is in that mask.
//
//   Worse than skipped: a GAME_REQUIRED model is not ON the list at all. Look at what
//   CStreaming::SetModelIsDeletable (0x409C10) does - it clears bit 0x02 at +6, and then, at
//   0x409C41, tests whether the entry's next-index is -1 and calls CStreamingInfo::AddToList to
//   put it BACK on the least-recently-used list. Clearing the flag is what makes a model
//   evictable again; nothing else does.
//
// San Andreas clears its own: CStreaming::RemoveCurrentZonesModels (0x40B080) is the call it
// makes when the popcycle zone changes, and it is why walking from one district to another does
// not slowly fill the streamer with everybody you have ever walked past. We never made that call.
//
// So every ped model a region ever asked for stayed resident for the session, out of the game's
// reach. Vice City alone names 67 groups over 43 districts. What that costs is not paid by the
// pedestrians - it is paid by whatever the streamer evicts INSTEAD, and the thing San Andreas
// evicts and reloads most is vehicles: CStreaming::LoadZoneVehicle brings in the cars for the
// player's zone and CStreaming::RemoveLoadedVehicle takes the old ones out. Starve that rotation
// and CCarCtrl::ChooseModel -> CLoadedCarGroup::PickLeastUsedModel hands the zone whatever
// happens to have survived, which is how a Feltzer turns up in Jefferson while an imported city
// hundreds of units away has nothing alive in it at all.
//
// WHAT THIS DOES INSTEAD. Ask with GAME_REQUIRED, because that is what gets a model loaded
// promptly and it is what the game asks its own zone peds with - and then, the moment it has
// loaded, call SetModelIsDeletable and forget it. From that point the model is protected exactly
// the way San Andreas' own are: by the peds standing in it. RemoveLeastUsedModel checks
// CBaseModelInfo::m_nRefCount at +8 (0x40D064) and will not touch a model with an instance in the
// world, so a model we are actually using cannot go anywhere, and a model we have stopped using
// can. That is the whole difference between borrowing and keeping.
#include <plugin.h>
#include <CStreaming.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "StreamedModels.h"
#include "ModelLimits.h"

using namespace plugin;

namespace {

// CStreaming::SetModelIsDeletable(int model), __cdecl, one argument. Called rather than patched,
// and checked before it is called for the same reason everything else here is: calling the wrong
// address is worse than not calling it. `push esi / mov esi, [esp+8] / lea eax, [esi+esi*4]` is
// the entry - the index arithmetic for a 0x14-byte CStreamingInfo.
constexpr uintptr_t kSetModelIsDeletable = 0x409C10;
const unsigned char kSetDeletablePrologue[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8D, 0x04, 0xB6 };

bool gChecked = false;
bool gUsable = false;

bool Usable() {
    if (!gChecked) {
        gChecked = true;
        gUsable = memcmp((const void*)kSetModelIsDeletable, kSetDeletablePrologue,
                         sizeof(kSetDeletablePrologue)) == 0;
    }
    return gUsable;
}

// The models we have asked for and not yet handed back. This is small by construction - it holds
// only what is in flight, because Tick() empties it as fast as the streamer fills the requests.
std::vector<int>& Outstanding() { static std::vector<int> v; return v; }

void Release(int model) {
    if (!Usable()) return;
    reinterpret_cast<void(__cdecl*)(int)>(kSetModelIsDeletable)(model);
}

}   // namespace

namespace StreamedModels {

void Want(int model) {
    if (model < 0) return;
    if (ModelLimits::ModelIsLoaded(model)) return;
    std::vector<int>& out = Outstanding();
    if (std::find(out.begin(), out.end(), model) != out.end()) return;   // already asked
    CStreaming::RequestModel(model, GAME_REQUIRED);
    out.push_back(model);
}

void Tick() {
    std::vector<int>& out = Outstanding();
    size_t write = 0;
    for (size_t i = 0; i < out.size(); i++) {
        if (ModelLimits::ModelIsLoaded(out[i])) { Release(out[i]); continue; }
        out[write++] = out[i];
    }
    out.resize(write);
}

void ReleaseAll() {
    std::vector<int>& out = Outstanding();
    for (int model : out) Release(model);
    out.clear();
}

int Held() { return (int)Outstanding().size(); }

}   // namespace StreamedModels
