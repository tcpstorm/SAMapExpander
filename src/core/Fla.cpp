// Noticing fastman92's limit adjuster, so that this plugin can get out of its way.
//
// It matters more than it looks. FLA ships an extended path format that lifts both of the limits
// this project ran into - a path node's int16 coordinates become int32, and a vehicle link's
// packed uint16 becomes a uint32 with 16 bits of node id and 16 of area - and it does it properly,
// by repointing San Andreas' own code: in its MapLimits.cpp the SA path patch alone is 609 pointer
// patches, 501 dword patches, 531 byte patches and 224 rewrites of `movsx word` into `mov dword`,
// with whole code blocks relocated. If it is doing that, the path system is its.
//
// THE TRAP THIS EXISTS TO CLOSE. src/limits/PathScale.cpp verifies its 288 sites before patching
// any of them, which reads like enough and is not. FLA rewrites the 16-bit LOADS that feed those
// multiplies, not the multiplies themselves - and it keeps the 0.125 at 0x858C48, because its
// extended coordinates are int32 at the same divide-by-eight scale. So under FLA those 288 sites
// still hold exactly the bytes PathScale expects, its verification passes, and it widens a scale
// that FLA needs left alone. Every coordinate in the game would come out wrong by that factor.
//
// So the encoding is checked directly instead, at the four places that say what it is. The test is
// deliberately behavioural rather than a search for a module: what must not happen is patching a
// path system somebody else has already changed, and it does not matter who changed it.
#include <plugin.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Fla.h"

namespace {

// It is named with a leading '$' on purpose, so that ASI loaders sort it first and it patches
// before anything else does. Which is also why we can never win a race with it.
const char* const kModule = "$fastman92limitAdjuster.asi";

// Four instructions that spell out the path encoding, with the operand each one carries in a stock
// executable. Verified against gta_sa.exe 1.0 US:
//     0041B9C4  shr  edx, 0xa           the navi link's area shift
//     0041B9FC  and  eax, 0x3ff         the navi link's index mask
//     00420ACC  imul eax, eax, 0x1c     sizeof(CPathNode)
//     0041BA01  imul eax, eax, 0xe      sizeof(CCarPathLink)
// FLA writes 16, 0xFFFF, 40/42/44 and 22/24 into them respectively.
constexpr uintptr_t kShiftSite = 0x41B9C6;   // imm8 of `shr edx, imm8`
constexpr uintptr_t kMaskSite = 0x41B9FD;    // imm32 of `and eax, imm32`
constexpr uintptr_t kNodeStrideSite = 0x420ACE;   // imm8 of `imul eax, eax, imm8`
constexpr uintptr_t kLinkStrideSite = 0x41BA03;   // imm8 of `imul eax, eax, imm8`

constexpr uint8_t kVanillaShift = 10;
constexpr uint32_t kVanillaMask = 0x3FF;
constexpr uint8_t kVanillaNodeStride = 0x1C;
constexpr uint8_t kVanillaLinkStride = 0x0E;

bool Readable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable) || (info.Protect & PAGE_GUARD)) return false;
    return address + bytes <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

struct Encoding {
    bool readable;
    uint8_t shift;
    uint32_t mask;
    uint8_t nodeStride;
    uint8_t linkStride;
};

Encoding Read() {
    Encoding e{};
    e.readable = Readable(kShiftSite, 1) && Readable(kMaskSite, 4) &&
                 Readable(kNodeStrideSite, 1) && Readable(kLinkStrideSite, 1);
    if (!e.readable) return e;
    memcpy(&e.shift, (const void*)kShiftSite, 1);
    memcpy(&e.mask, (const void*)kMaskSite, 4);
    memcpy(&e.nodeStride, (const void*)kNodeStrideSite, 1);
    memcpy(&e.linkStride, (const void*)kLinkStrideSite, 1);
    return e;
}

}   // namespace

namespace Fla {

bool Loaded() { return GetModuleHandleA(kModule) != nullptr; }

std::string Version() {
    HMODULE module = GetModuleHandleA(kModule);
    if (!module) return std::string();
    typedef const char*(__cdecl* GetProjectVersionFn)();
    auto get = reinterpret_cast<GetProjectVersionFn>(GetProcAddress(module, "GetProjectVersion"));
    if (!get) return std::string();
    const char* version = get();
    return version ? std::string(version) : std::string();
}

bool PathEncodingChanged() {
    Encoding e = Read();
    // Unreadable is treated as changed, which is the safe way round: it means something is not
    // where this expects it, and that is not a moment to start writing to the path system.
    if (!e.readable) return true;
    return e.shift != kVanillaShift || e.mask != kVanillaMask ||
           e.nodeStride != kVanillaNodeStride || e.linkStride != kVanillaLinkStride;
}

void DescribePathEncoding(std::string& out) {
    Encoding e = Read();
    char buffer[256];
    if (!e.readable) {
        out = "the path encoding sites cannot be read";
        return;
    }
    snprintf(buffer, sizeof(buffer),
             "navi link shift %u, mask 0x%X, CPathNode %u bytes, CCarPathLink %u bytes%s",
             e.shift, e.mask, e.nodeStride, e.linkStride,
             PathEncodingChanged() ? " - NOT what San Andreas shipped" : " (vanilla)");
    out = buffer;
}

}   // namespace Fla
