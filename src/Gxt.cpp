// Patches strings into the game's text lookup: whole GXT files from [World] Gxt (a region's
// original american.gxt, in III/VC or SA format) plus anything the other modules register.
//
// SA looks a string up by CKeyGen::GetUppercaseKey -- CRC32 (0xEDB88320, no final inversion) over
// the upper-cased key -- so our table is keyed by the same hash. That is also the only thing an
// SA-format GXT stores: its TKEY entries carry the hash, not the name.
#include <plugin.h>
#include <safetyhook.hpp>
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

std::unordered_map<uint32_t, Entry> gEntries;
std::unordered_set<uint32_t> gMissesSeen;

SafetyHookInline gTextGetHook;
bool gAudited = false;
bool gLogMisses = false;
int gMissesLogged = 0;
int gConflicts = 0;
int gReplacements = 0;

const char* gMissingText = nullptr;   // what CText::Get answers for a key nobody has

const char* CallOriginal(const char* key) {
    return gTextGetHook.thiscall<const char*>(reinterpret_cast<void*>(kTheText), key);
}

// ---------------------------------------------------------------- the game's own table

const GameKeyEntry* GameEntries() { return *reinterpret_cast<GameKeyEntry**>(kTheText); }
uint32_t GameEntryCount() { return *reinterpret_cast<uint32_t*>(kTheText + 4); }

// CKeyArray::BinarySearch relies on the block being sorted by hash, so we can do the same.
const GameKeyEntry* FindGameEntry(uint32_t hash) {
    const GameKeyEntry* entries = GameEntries();
    uint32_t count = GameEntryCount();
    if (!entries || !count) return nullptr;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid].hash == hash) return &entries[mid];
        if (entries[mid].hash < hash) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// ---------------------------------------------------------------- table

bool AddHashed(uint32_t hash, const std::string& name, const std::string& text, const std::string& source) {
    auto it = gEntries.find(hash);
    if (it != gEntries.end()) {
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
        return true;
    }
    gEntries.emplace(hash, Entry{ text, name, source });
    return true;
}

// ---------------------------------------------------------------- hook

const char* __fastcall Hooked_TextGet(void* self, void* /*edx*/, const char* key);

void Audit();

bool gHookTried = false;

void EnsureHook() {
    if (gHookTried) return;
    gHookTried = true;
    Log::Write("GXT: hooking CText::Get at 0x%X", (unsigned)kTextGet);
    gTextGetHook = safetyhook::create_inline(reinterpret_cast<void*>(kTextGet),
                                             reinterpret_cast<void*>(&Hooked_TextGet));
    if (!gTextGetHook)
        Log::Write("GXT: could not hook CText::Get; no string of ours will reach the game");
}

void NoteMiss(const char* key) {
    if (gMissesLogged >= 50) return;
    uint32_t hash = HashKey(key);
    if (!gMissesSeen.insert(hash).second) return;
    char safe[16] = { 0 };
    for (int i = 0; i < 15 && key[i]; i++) safe[i] = key[i];
    gMissesLogged++;
    Log::Write("GXT miss: '%s' is in neither the game's GXT nor ours", safe);
    if (gMissesLogged == 50) Log::Write("GXT: 50 missing keys logged, no more will be reported");
}

const char* __fastcall Hooked_TextGet(void* self, void* /*edx*/, const char* key) {
    if (!gAudited) Audit();
    if (key && key[0] && key[0] != ' ') {
        auto it = gEntries.find(HashKey(key));
        if (it != gEntries.end()) return it->second.text.c_str();
    }
    // unsafe_ skips safetyhook's per-call mutex; this runs many times per frame and the hook lives
    // until the process ends, so there is nothing to guard against.
    const char* result = gTextGetHook.unsafe_thiscall<const char*>(self, key);
    if (gLogMisses && key && result == gMissingText) NoteMiss(key);
    return result;
}

// Runs once, on the first lookup after american.gxt is in memory: only then can we say which of our
// keys the game also has.
void Audit() {
    if (GameEntryCount() == 0) return;
    gAudited = true;

    gMissingText = CallOriginal("\x01ZZZZZZ");

    int overrides = 0, same = 0;
    for (const auto& kv : gEntries) {
        const GameKeyEntry* theirs = FindGameEntry(kv.first);
        if (!theirs || !theirs->text) continue;
        if (kv.second.text == theirs->text) { same++; continue; }
        overrides++;
        if (overrides <= 20) {
            char label[24];
            if (kv.second.name.empty()) snprintf(label, sizeof(label), "#%08X", kv.first);
            else snprintf(label, sizeof(label), "%s", kv.second.name.c_str());
            Log::Write("GXT: %s overrides \"%s\" with \"%s\"", label, theirs->text, kv.second.text.c_str());
        }
    }
    if (overrides > 20) Log::Write("GXT: ... and %d more overridden key(s)", overrides - 20);
    Log::Write("GXT: %d of our %u key(s) override the game's text, %d repeat it unchanged",
               overrides, (unsigned)gEntries.size(), same);
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
        return gEntries.count(HashKey(key)) != 0;
    }

    void Install() {
        gLogMisses = Settings::world.gxtLogMisses;
        if (Settings::world.gxtFiles.empty()) return;

        int files = 0, entries = 0;
        for (const std::string& path : Settings::world.gxtFiles) {
            int added = 0;
            if (LoadGxtFile(path, added)) { files++; entries += added; }
        }
        if (!gEntries.empty()) EnsureHook();
        Log::Write("GXT: %d of %u file(s) loaded, %d entries, %d duplicate key(s), %d character(s) replaced%s",
                   files, (unsigned)Settings::world.gxtFiles.size(), entries, gConflicts, gReplacements,
                   gLogMisses ? ", missing-key logging on" : "");
    }
}
