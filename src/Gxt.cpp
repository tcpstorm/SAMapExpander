// Patches strings into the game's text lookup: whole GXT files from [World] Gxt (a region's
// original american.gxt, in III/VC or SA format) plus anything the other modules register.
//
// SA looks a string up by CKeyGen::GetUppercaseKey -- CRC32 (0xEDB88320, no final inversion) over
// the upper-cased key -- so our table is keyed by the same hash. That is also the only thing an
// SA-format GXT stores: its TKEY entries carry the hash, not the name.
#include <plugin.h>
#include <safetyhook.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Gxt.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kTextGet = 0x6A0050;   // CText::Get(const char*), __thiscall
constexpr uintptr_t kTheText = 0xC1B340;   // TheText; CKeyArray m_MainKeyArray is its first member

// One entry of the game's own TKEY block, after CKeyArray::Update turned the offsets into pointers.
struct GameKeyEntry {
    const char* text;
    uint32_t    hash;
};

struct Entry {
    std::string text;
    std::string name;    // empty for SA-format keys, which only carry a hash
    std::string source;
};

constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

uint32_t HashKey(const char* s) {
    uint32_t key = 0xFFFFFFFFu;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);
        key = kCrcTable[(uint8_t)((uint8_t)c ^ key)] ^ (key >> 8);
    }
    return key;
}

// Install() is called from Main.cpp's static constructor, i.e. during the DLL's own static
// initialisation, and this file's globals may not be constructed yet at that point (MinGW runs
// constructors in reverse link order). Function-local statics are constructed on first use.
std::unordered_map<uint32_t, Entry>& Entries() { static std::unordered_map<uint32_t, Entry> m; return m; }
std::unordered_set<uint32_t>& MissesSeen() { static std::unordered_set<uint32_t> s; return s; }

// Same static-initialisation caveat as Entries(): a SafetyHookInline holds a mutex and an
// allocator handle, and assigning into a never-constructed one crashes inside create_inline.
SafetyHookInline& TextGetHook() { static SafetyHookInline h; return h; }
SafetyHookInline& TextLoadHook() { static SafetyHookInline h; return h; }
bool gLogMisses = false;
int gMissesLogged = 0;
int gConflicts = 0;
int gReplacements = 0;
bool gTableDirty = false;         // AddString after the merge: rebuild on the next sync

const char* gMissingText = nullptr;   // what CText::Get answers for a key nobody has

const char* CallGameGet(const char* key) {
    return reinterpret_cast<const char*(__thiscall*)(void*, const char*)>(kTextGet)(
        reinterpret_cast<void*>(kTheText), key);
}

// ---------------------------------------------------------------- the game's own table
//
// TheText starts with CKeyArray m_MainKeyArray = { CKeyEntry* entries; int count; }. CText::Get
// hashes the key and binary-searches that block (CKeyArray::BinarySearch, int16 indices), so a
// string we merge into the block is found by every caller, whatever else has been done to
// CText::Get itself. We used to hook CText::Get instead; SA-MP and at least one other mod hook it
// after us without relocating our jump properly, which lost our strings or crashed the game.
//
// CKeyArray::Load allocates the block with the exe's malloc (0x821195) and CText::Unload frees it
// with its free (0x8213AE), so our replacement block comes from the same allocator.

struct KeyArray {
    GameKeyEntry* entries;
    uint32_t      count;
};
KeyArray& MainKeyArray() { return *reinterpret_cast<KeyArray*>(kTheText); }

constexpr uintptr_t kGameMalloc = 0x821195;
constexpr uintptr_t kGameFree = 0x8213AE;
constexpr uint32_t kMaxEntries = 32767;   // CKeyArray::BinarySearch walks the block with int16 indices

void* GameMalloc(size_t bytes) { return reinterpret_cast<void*(__cdecl*)(size_t)>(kGameMalloc)(bytes); }
void GameFree(void* p) { reinterpret_cast<void(__cdecl*)(void*)>(kGameFree)(p); }

// The block CText::Load produced, before our entries went in: the audit compares against it and
// every rebuild merges from it.
std::vector<GameKeyEntry>& GameOriginal() { static std::vector<GameKeyEntry> v; return v; }
GameKeyEntry* gInjectedBlock = nullptr;
int gSyncsLogged = 0;

// A key only we define; its presence (pointing at our text) proves the live block is ours.
constexpr const char* kSentinelKey = "MXTABLE";

// CKeyArray::BinarySearch relies on the block being sorted by hash, so we can do the same.
const GameKeyEntry* FindIn(const GameKeyEntry* entries, uint32_t count, uint32_t hash) {
    if (!entries || !count) return nullptr;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid].hash == hash) return &entries[mid];
        if (entries[mid].hash < hash) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

const GameKeyEntry* FindGameEntry(uint32_t hash) {
    return FindIn(GameOriginal().data(), (uint32_t)GameOriginal().size(), hash);
}

bool LiveTableIsOurs() {
    const KeyArray& live = MainKeyArray();
    auto ours = Entries().find(HashKey(kSentinelKey));
    if (ours == Entries().end()) return false;
    const GameKeyEntry* e = FindIn(live.entries, live.count, ours->first);
    return e && e->text == ours->second.text.c_str();
}

// ---------------------------------------------------------------- table

bool AddHashed(uint32_t hash, const std::string& name, const std::string& text, const std::string& source) {
    auto it = Entries().find(hash);
    if (it != Entries().end()) {
        if (it->second.text != text) {
            gConflicts++;
            if (gConflicts <= 20) {
                const std::string& known = name.empty() ? it->second.name : name;
                char label[24];
                if (known.empty()) snprintf(label, sizeof(label), "#%08X", hash);
                else snprintf(label, sizeof(label), "%s", known.c_str());
                Log::Write("GXT: %s defined twice: %s says \"%s\", %s says \"%s\" (the later one wins)",
                           label, it->second.source.c_str(), it->second.text.c_str(),
                           source.c_str(), text.c_str());
            }
        }
        it->second = Entry{ text, name.empty() ? it->second.name : name, source };
        gTableDirty = true;
        return true;
    }
    Entries().emplace(hash, Entry{ text, name, source });
    gTableDirty = true;
    return true;
}

// ---------------------------------------------------------------- hook

const char* __fastcall Hooked_TextGet(void* self, void* /*edx*/, const char* key);
void __fastcall Hooked_TextLoad(void* self, void* /*edx*/, bool keepMissionPack);

bool gHookTried = false;
constexpr uintptr_t kTextLoad = 0x6A01A0;   // CText::Load(bool keepMissionPack), __thiscall

void SyncTable();

void EnsureHook() {
    if (gHookTried) return;
    gHookTried = true;
    AddHashed(HashKey(kSentinelKey), kSentinelKey, "MapExpander", "sentinel");
    Log::Write("GXT: hooking CText::Load at 0x%X%s", (unsigned)kTextLoad,
               gLogMisses ? " and CText::Get at 0x6A0050 for missing-key logging" : "");
    TextLoadHook() = safetyhook::create_inline(reinterpret_cast<void*>(kTextLoad),
                                              reinterpret_cast<void*>(&Hooked_TextLoad));
    if (!TextLoadHook())
        Log::Write("GXT: could not hook CText::Load; our strings go in on the first frame instead");
    if (gLogMisses) {
        TextGetHook() = safetyhook::create_inline(reinterpret_cast<void*>(kTextGet),
                                                 reinterpret_cast<void*>(&Hooked_TextGet));
        if (!TextGetHook()) Log::Write("GXT: could not hook CText::Get; missing-key logging is off");
    }
}

void NoteMiss(const char* key) {
    if (gMissesLogged >= 50) return;
    uint32_t hash = HashKey(key);
    if (!MissesSeen().insert(hash).second) return;
    char safe[16] = { 0 };
    for (int i = 0; i < 15 && key[i]; i++) safe[i] = key[i];
    gMissesLogged++;
    Log::Write("GXT miss: '%s' is in neither the game's GXT nor ours", safe);
    if (gMissesLogged == 50) Log::Write("GXT: 50 missing keys logged, no more will be reported");
}

// Only installed for GxtLogMisses: our strings reach the game through its table, not through here.
const char* __fastcall Hooked_TextGet(void* self, void* /*edx*/, const char* key) {
    // unsafe_ skips safetyhook's per-call mutex; this runs many times per frame and the hook lives
    // until the process ends, so there is nothing to guard against.
    const char* result = TextGetHook().unsafe_thiscall<const char*>(self, key);
    if (gMissingText && key && key[0] && key[0] != ' ' && result == gMissingText) NoteMiss(key);
    return result;
}

void __fastcall Hooked_TextLoad(void* self, void* /*edx*/, bool keepMissionPack) {
    TextLoadHook().thiscall<void>(self, keepMissionPack);
    SyncTable();
}

// Compares our keys against the block CText::Load just produced (real pointers by now): which of
// ours the game also has, and what it answers for a key nobody has.
void Audit(bool verbose) {
    int overrides = 0, same = 0;
    for (const auto& kv : Entries()) {
        const GameKeyEntry* theirs = FindGameEntry(kv.first);
        if (!theirs || !theirs->text) continue;
        if (kv.second.text == theirs->text) { same++; continue; }
        overrides++;
        if (verbose && overrides <= 20) {
            char label[24];
            if (kv.second.name.empty()) snprintf(label, sizeof(label), "#%08X", kv.first);
            else snprintf(label, sizeof(label), "%s", kv.second.name.c_str());
            Log::Write("GXT: %s overrides \"%s\" with \"%s\"", label, theirs->text, kv.second.text.c_str());
        }
    }
    if (verbose && overrides > 20) Log::Write("GXT: ... and %d more overridden key(s)", overrides - 20);
    Log::Write("GXT: %d of our %u key(s) override the game's text, %d repeat it unchanged",
               overrides, (unsigned)Entries().size(), same);
}

// Builds game-original + ours (ours wins on a shared hash) and hands the block to TheText.
void InstallMergedTable() {
    std::vector<GameKeyEntry> ours;
    ours.reserve(Entries().size());
    for (const auto& kv : Entries()) ours.push_back(GameKeyEntry{ kv.second.text.c_str(), kv.first });
    std::sort(ours.begin(), ours.end(),
              [](const GameKeyEntry& a, const GameKeyEntry& b) { return a.hash < b.hash; });

    const std::vector<GameKeyEntry>& game = GameOriginal();
    std::vector<GameKeyEntry> merged;
    merged.reserve(game.size() + ours.size());
    size_t g = 0, o = 0;
    int replaced = 0;
    while (g < game.size() || o < ours.size()) {
        if (o == ours.size() || (g < game.size() && game[g].hash < ours[o].hash)) {
            merged.push_back(game[g++]);
        } else {
            if (g < game.size() && game[g].hash == ours[o].hash) { g++; replaced++; }
            merged.push_back(ours[o++]);
        }
    }

    int dropped = 0;
    if (merged.size() > kMaxEntries) {
        dropped = (int)(merged.size() - kMaxEntries);
        merged.resize(kMaxEntries);
    }

    GameKeyEntry* block = static_cast<GameKeyEntry*>(GameMalloc(merged.size() * sizeof(GameKeyEntry)));
    if (!block) {
        Log::Write("GXT: the game's allocator refused %u bytes; our strings are not in its table",
                   (unsigned)(merged.size() * sizeof(GameKeyEntry)));
        return;
    }
    memcpy(block, merged.data(), merged.size() * sizeof(GameKeyEntry));

    KeyArray& live = MainKeyArray();
    GameKeyEntry* previous = live.entries;
    live.entries = block;
    live.count = (uint32_t)merged.size();
    // Our own earlier block can go; the game's original stays allocated (something else may still
    // point at it, and it is a few dozen KB at most).
    if (previous && previous == gInjectedBlock) GameFree(previous);
    gInjectedBlock = block;
    gTableDirty = false;

    if (gSyncsLogged < 5) {
        gSyncsLogged++;
        Log::Write("GXT: game table rebuilt: %u of the game's keys + %u of ours (%d replaced) = %u entries%s",
                   (unsigned)game.size(), (unsigned)ours.size(), replaced, (unsigned)merged.size(),
                   dropped ? " (TRUNCATED: the table cannot hold more than 32767 keys)" : "");
        if (dropped) Log::Write("GXT: %d key(s) with the highest hashes were dropped", dropped);
    }
}

// Makes sure the live MAIN table carries our strings. Called after every CText::Load and once per
// frame (cheap when nothing changed): a text reload we did not see, or a mod that replaced the
// block, is caught on the next frame.
void SyncTable() {
    if (Entries().empty()) return;
    const KeyArray& live = MainKeyArray();
    if (!live.entries || live.count == 0) return;   // nothing loaded yet

    if (!LiveTableIsOurs()) {
        // A fresh block from CText::Load: snapshot it, report overrides against it, then merge.
        GameOriginal().assign(live.entries, live.entries + live.count);
        static bool auditedOnce = false;
        Audit(!auditedOnce);
        auditedOnce = true;
        InstallMergedTable();
        gMissingText = CallGameGet("\x01ZZZZZZ");
    } else if (gTableDirty) {
        InstallMergedTable();
    }
}

// ---------------------------------------------------------------- file parsing

struct Reader {
    const std::vector<uint8_t>& data;
    const std::string& path;
    bool ok = true;

    bool Has(size_t at, size_t bytes) const { return at + bytes <= data.size(); }
    uint16_t U16(size_t at) const { return (uint16_t)(data[at] | (data[at + 1] << 8)); }
    uint32_t U32(size_t at) const {
        return (uint32_t)data[at] | ((uint32_t)data[at + 1] << 8) |
               ((uint32_t)data[at + 2] << 16) | ((uint32_t)data[at + 3] << 24);
    }
    bool Magic(size_t at, const char* what) const {
        return Has(at, 4) && memcmp(&data[at], what, 4) == 0;
    }
    void Fail(size_t at, const char* what) {
        Log::Write("GXT: %s: %s at byte %u; the rest of the file is not read", path.c_str(), what,
                   (unsigned)at);
        ok = false;
    }
};

int gKeyReplacementLogs = 0;

// III/VC store text as 16-bit characters. SA's font is 8-bit: ASCII and the Latin-1 range pass
// through unchanged (the code points line up), anything else becomes '?'.
std::string Convert16(const uint8_t* p, size_t bytes, const char* keyName, const std::string& path) {
    std::string out;
    int replaced = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t c = (uint16_t)(p[i] | (p[i + 1] << 8));
        if (!c) break;
        if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) out.push_back((char)c);
        else { out.push_back('?'); replaced++; }
    }
    if (replaced) {
        gReplacements += replaced;
        if (gKeyReplacementLogs < 8) {
            gKeyReplacementLogs++;
            Log::Write("GXT: %s: %s has %d character(s) SA cannot show, replaced with '?': \"%s\"",
                       path.c_str(), keyName, replaced, out.c_str());
        }
    }
    return out;
}

std::string Convert8(const uint8_t* p, size_t bytes) {
    std::string out;
    for (size_t i = 0; i < bytes && p[i]; i++) out.push_back((char)p[i]);
    return out;
}

// A TKEY/TDAT pair. `entrySize` is 12 for III/VC (offset + 8 character name) and 8 for SA
// (offset + hash of the name).
int LoadTable(Reader& r, size_t keyBlock, size_t keySize, size_t datBlock, size_t datSize,
              unsigned entrySize, unsigned bitsPerChar) {
    int added = 0, skipped = 0;
    const uint8_t* dat = r.data.data() + datBlock;
    for (size_t at = keyBlock; at + entrySize <= keyBlock + keySize; at += entrySize) {
        uint32_t offset = r.U32(at);
        if (offset >= datSize) {
            if (skipped < 8)
                Log::Write("GXT: %s: entry at byte %u points past TDAT (offset %u of %u); skipped",
                           r.path.c_str(), (unsigned)at, offset, (unsigned)datSize);
            skipped++;
            continue;
        }

        std::string name;
        uint32_t hash;
        if (entrySize == 12) {
            const char* raw = reinterpret_cast<const char*>(&r.data[at + 4]);
            size_t len = 0;
            while (len < 8 && raw[len]) len++;
            if (len == 0 || len > 7) {
                if (skipped < 8)
                    Log::Write("GXT: %s: unusable key at byte %u (%u characters); skipped",
                               r.path.c_str(), (unsigned)at, (unsigned)len);
                skipped++;
                continue;
            }
            name.assign(raw, len);
            for (char& c : name) c = (char)toupper((unsigned char)c);
            hash = HashKey(name.c_str());
        } else {
            hash = r.U32(at + 4);
        }

        size_t room = datSize - offset;
        std::string text = bitsPerChar == 16 ? Convert16(dat + offset, room,
                                                         name.empty() ? "a hash-only key" : name.c_str(), r.path)
                                             : Convert8(dat + offset, room);
        AddHashed(hash, name, text, r.path);
        added++;
    }
    if (skipped > 8) Log::Write("GXT: %s: %d unusable entries in total", r.path.c_str(), skipped);
    return added;
}

// Reads the TKEY+TDAT pair that starts at `at` (after an optional 8 byte table name).
int LoadTablePair(Reader& r, size_t at, unsigned entrySize, unsigned bitsPerChar, size_t* endOut) {
    if (!r.Magic(at, "TKEY") && r.Has(at + 8, 4) && r.Magic(at + 8, "TKEY")) at += 8;  // table name
    if (!r.Magic(at, "TKEY") || !r.Has(at, 8)) { r.Fail(at, "expected a TKEY block"); return 0; }
    uint32_t keySize = r.U32(at + 4);
    size_t keyBlock = at + 8;
    if (!r.Has(keyBlock, keySize)) { r.Fail(at, "TKEY block runs past the end of the file"); return 0; }

    size_t datHeader = keyBlock + keySize;
    if (!r.Magic(datHeader, "TDAT") || !r.Has(datHeader, 8)) { r.Fail(datHeader, "expected a TDAT block"); return 0; }
    uint32_t datSize = r.U32(datHeader + 4);
    size_t datBlock = datHeader + 8;
    if (!r.Has(datBlock, datSize)) { r.Fail(datHeader, "TDAT block runs past the end of the file"); return 0; }

    if (endOut) *endOut = datBlock + datSize;
    return LoadTable(r, keyBlock, keySize, datBlock, datSize, entrySize, bitsPerChar);
}

bool LoadGxtFile(const std::string& path, int& added) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Log::Write("GXT: %s not found", path.c_str());
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 16) {
        Log::Write("GXT: %s is too small to be a GXT file; ignored", path.c_str());
        return false;
    }

    Reader r{ data, path };

    if (r.Magic(0, "TKEY")) {
        // GTA III: no file header, one TKEY/TDAT pair, 16 bit characters.
        added += LoadTablePair(r, 0, 12, 16, nullptr);
        if (!r.ok) return false;
        Log::Write("GXT: %s: III format, %d entries", path.c_str(), added);
        return true;
    }

    if (r.Magic(0, "TABL")) {
        // Vice City: no file header either, but a TABL of named tables (MAIN first, unprefixed;
        // the rest carry their 8 byte name), 12 byte named keys and 16 bit characters.
        uint32_t tablSize = r.U32(4);
        size_t tabl = 8;
        if (!r.Has(tabl, tablSize)) { r.Fail(0, "TABL block runs past the end of the file"); return false; }
        std::vector<size_t> tables;
        for (size_t e = tabl; e + 12 <= tabl + tablSize; e += 12) {
            uint32_t offset = r.U32(e + 8);
            if (offset + 8 <= data.size()) tables.push_back(offset);
            else Log::Write("GXT: %s: table entry at byte %u points past the file; skipped",
                            path.c_str(), (unsigned)e);
        }
        if (tables.empty()) { r.Fail(0, "TABL lists no tables"); return false; }
        for (size_t offset : tables) {
            size_t end = 0;
            added += LoadTablePair(r, offset, 12, 16, &end);
            if (!r.ok) return false;
        }
        Log::Write("GXT: %s: VC format, %u table(s), %d entries", path.c_str(), (unsigned)tables.size(), added);
        return true;
    }

    uint16_t version = r.U16(0);
    uint16_t bits = r.U16(2);
    if (bits != 8 && bits != 16) {
        Log::Write("GXT: %s: unknown format (first bytes %02X %02X %02X %02X); ignored",
                   path.c_str(), data[0], data[1], data[2], data[3]);
        return false;
    }

    std::vector<size_t> tables;
    size_t at = 4;
    if (r.Magic(at, "TABL") && r.Has(at, 8)) {
        uint32_t tablSize = r.U32(at + 4);
        size_t tabl = at + 8;
        if (!r.Has(tabl, tablSize)) { r.Fail(at, "TABL block runs past the end of the file"); return false; }
        for (size_t e = tabl; e + 12 <= tabl + tablSize; e += 12) {
            uint32_t offset = r.U32(e + 8);
            if (offset + 8 <= data.size()) tables.push_back(offset);
            else Log::Write("GXT: %s: table entry at byte %u points past the file; skipped",
                            path.c_str(), (unsigned)e);
        }
        at = tabl + tablSize;
    }

    size_t mainEnd = 0;
    added += LoadTablePair(r, at, 8, bits, &mainEnd);
    if (!r.ok) return false;
    for (size_t offset : tables) {
        if (offset >= at && offset < mainEnd) continue;  // the main table, already read
        size_t end = 0;
        added += LoadTablePair(r, offset, 8, bits, &end);
        if (!r.ok) return false;
    }

    Log::Write("GXT: %s: SA format (version %u, %u bit), %u table(s), %d entries",
               path.c_str(), version, bits, (unsigned)(tables.empty() ? 1 : tables.size()), added);
    return true;
}

}  // namespace

namespace Gxt {
    bool AddString(const char* key, const char* text, const char* source) {
        if (!key || !key[0] || !text) return false;
        char upper[8] = { 0 };
        size_t len = 0;
        while (len < 8 && key[len]) len++;
        if (len > 7) {
            Log::Write("GXT: key '%.8s' from %s is longer than 7 characters; skipped", key, source);
            return false;
        }
        for (size_t i = 0; i < len; i++) upper[i] = (char)toupper((unsigned char)key[i]);
        EnsureHook();
        return AddHashed(HashKey(upper), upper, text, source);
    }

    bool Has(const char* key) {
        if (!key || !key[0]) return false;
        return Entries().count(HashKey(key)) != 0;
    }

    const char* Find(const char* key) {
        if (!key || !key[0]) return nullptr;
        auto it = Entries().find(HashKey(key));
        return it == Entries().end() ? nullptr : it->second.text.c_str();
    }

    void EnsureInjected() {
        SyncTable();
    }

    void Install() {
        gLogMisses = Settings::world.gxtLogMisses;
        if (Settings::world.gxtFiles.empty()) return;

        int files = 0, entries = 0;
        for (const std::string& path : Settings::world.gxtFiles) {
            int added = 0;
            if (LoadGxtFile(path, added)) { files++; entries += added; }
        }
        if (!Entries().empty()) EnsureHook();
        Log::Write("GXT: %d of %u file(s) loaded, %d entries, %d duplicate key(s), %d character(s) replaced%s",
                   files, (unsigned)Settings::world.gxtFiles.size(), entries, gConflicts, gReplacements,
                   gLogMisses ? ", missing-key logging on" : "");
    }
}
