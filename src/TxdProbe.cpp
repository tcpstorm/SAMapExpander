// Which texture dictionaries actually load, and with what in them.
//
// A material whose texture RenderWare cannot find at DFF read time is drawn in its material colour,
// which is white. So "white leaves" means the leaf texture was not found in the dictionary that was
// current when the tree's DFF was read - and every explanation for that is a fact about the
// dictionary: it was never loaded, it loaded empty, it loaded under a slot the model does not use,
// or it was pre-empted by a slot the game had already created under the same name. The offline
// checks say the archive is right; this says what the game did with it.
//
// CTxdStore keeps no names, only CKeyGen hashes, so the name is caught on its way in at
// AddTxdSlot, and reported back out when CStreaming::ConvertBufferToObject hands the slot's stream
// to CTxdStore::LoadTxd.
#include <plugin.h>
#include <safetyhook.hpp>
#include <map>
#include <string>
#include "TxdProbe.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kAddTxdSlot = 0x731C80;   // int CTxdStore::AddTxdSlot(const char* name)
constexpr uintptr_t kLoadTxd = 0x731DD0;      // bool CTxdStore::LoadTxd(int slot, RwStream* stream)
constexpr uintptr_t kGetTxd = 0x408340;       // RwTexDictionary* CTxdStore::GetTxd(int slot)

static SafetyHookInline& AddSlotHook() { static SafetyHookInline h; return h; }
static SafetyHookInline& LoadHook() { static SafetyHookInline h; return h; }
static std::map<int, std::string>& SlotNames() { static std::map<int, std::string> m; return m; }
static int gLogged = 0;
const int kMaxLogged = 400;

int __cdecl Hooked_AddTxdSlot(const char* name) {
    const int slot = AddSlotHook().ccall<int>(name);
    if (slot >= 0 && name) SlotNames()[slot] = name;
    return slot;
}

RwTexture* CountTexture(RwTexture* texture, void* data) {
    (*static_cast<int*>(data))++;
    return texture;
}

bool __cdecl Hooked_LoadTxd(int slot, RwStream* stream) {
    const bool ok = LoadHook().ccall<bool>(slot, stream);
    if (gLogged >= kMaxLogged) return ok;

    auto it = SlotNames().find(slot);
    const std::string name = it == SlotNames().end() ? std::string("?") : it->second;
    // Every dictionary the game itself creates was added before this hook existed, so the ones it
    // knows the name of are almost all a region's - which is what is worth reading.
    if (it == SlotNames().end()) return ok;

    int count = 0;
    typedef RwTexDictionary*(__cdecl* GetTxdFn)(int);
    RwTexDictionary* dict = reinterpret_cast<GetTxdFn>(kGetTxd)(slot);
    if (dict) RwTexDictionaryForAllTextures(dict, CountTexture, &count);

    gLogged++;
    Log::Write("txd: slot %d \"%s\" %s, %d texture(s)%s", slot, name.c_str(),
               ok ? "loaded" : "FAILED to load", count,
               (ok && count == 0) ? "  <- empty: every material bound to it will draw white" : "");
    if (gLogged == kMaxLogged) Log::Write("txd: (no more dictionary loads logged)");
    return ok;
}

}  // namespace

namespace TxdProbe {

void Install() {
    if (!Settings::world.debug) return;
    AddSlotHook() = safetyhook::create_inline(reinterpret_cast<void*>(kAddTxdSlot),
                                              reinterpret_cast<void*>(&Hooked_AddTxdSlot));
    LoadHook() = safetyhook::create_inline(reinterpret_cast<void*>(kLoadTxd),
                                           reinterpret_cast<void*>(&Hooked_LoadTxd));
    if (!AddSlotHook() || !LoadHook())
        Log::Write("txd probe: could not hook CTxdStore; dictionary loads will not be logged");
}

}  // namespace TxdProbe
