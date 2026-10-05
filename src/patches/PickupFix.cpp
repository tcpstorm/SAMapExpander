// CPickup and CCarGenerator store positions as int16 * 1/8 (range +/-4095.9). The game inlines
// the conversion everywhere via two shared constants (8.0 at 0x859000, 0.125 at 0x858C48), so we
// point each of those sites at our own scale instead, trading precision for range.
//
// The conversion itself is `(int16)(x * scale)`, through the CRT's _ftol, which truncates toward
// zero. So a pickup never sits where it was asked to put but up to one step nearer the origin on
// each axis - 0.125 at the vanilla scale, which nobody noticed, 0.25 at the default 4, which puts
// a pickup visibly beside the 3D text label somebody placed on the same float coordinates. The
// two functions that compress a position - CPickups::GenerateNewOne and CPickup::SetPosn - are
// therefore wrapped and each coordinate pushed half a step away from zero on its way in, which
// turns the truncation into rounding to nearest and halves the worst case to what vanilla had.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cmath>
#include <cstdint>
#include "PickupFix.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

static float gCompress = 8.0f;
static float gDecompress = 0.125f;

static const uint32_t ORIGINAL_COMPRESS = 0x859000;
static const uint32_t ORIGINAL_DECOMPRESS = 0x858C48;

// fmul dword ptr [imm32]: D8 0D disp32
static const unsigned FMUL_OPCODE_LEN = 2;

static const uintptr_t PICKUP_DECOMPRESS_SITES[] = {
    0x4549B9, 0x4549CB, 0x4549D5,           // CPickup::GetPosn
    0x4549FD, 0x454A1D, 0x454A3D,           // CPickup::GetX/Y/ZCoord
    0x454C8E, 0x454C98,                     // CPickup::IsVisible
    0x4555E6, 0x4555F4, 0x4555FE,           // CPickups::TryToMerge_WeaponType
    0x4563D2, 0x4563E0, 0x4563EA,           // CPickups::RemoveUnnecessaryPickups
    0x456477, 0x456485, 0x45648F,           // CPickups::TestForPickupsInBubble
    0x456AC4, 0x456AD7, 0x456AE1,           // CPickups::PictureTaken
    0x456B61, 0x456B78, 0x456B86,
    0x456D56, 0x456D7E, 0x456DA6,           // CPickups::RemovePickUpsInArea
    0x457444, 0x457457, 0x457465,           // CPickup::Update
    0x4574C8, 0x4574E5, 0x4574F3,
    0x458769, 0x45877B,
};

static const uintptr_t PICKUP_COMPRESS_SITES[] = {
    0x454965, 0x454976, 0x454989,           // CPickup::SetPosn
    0x4572B0, 0x4572D1, 0x4572E4,           // CPickups::GenerateNewOne
};

static const uintptr_t CARGEN_DECOMPRESS_SITES[] = {
    0x6F2F6F, 0x6F2F8D, 0x6F2FA6,           // CCarGenerator::CheckIfWithinRangeOfAnyPlayers
    0x6F3069, 0x6F3081, 0x6F3096, 0x6F3113,
    0x6F332E, 0x6F334D, 0x6F3367,           // CCarGenerator::CheckForBlockage
    0x6F33D7, 0x6F3455,
    0x6F36AE, 0x6F36BC, 0x6F36CA,           // CCarGenerator::DoInternalProcessing
    0x6F3A80, 0x6F3AAE, 0x6F3ABC, 0x6F3B19, 0x6F3B27,
};

static const uintptr_t CARGEN_COMPRESS_SITES[] = {
    0x6F2E55, 0x6F2E66, 0x6F2E79,           // CCarGenerator::Setup
};

template <size_t N>
static int Retarget(const uintptr_t (&sites)[N], uint32_t expectedOld, const float* target) {
    int patched = 0;
    for (uintptr_t site : sites)
        if (MemPatch::RetargetDisp32(site, FMUL_OPCODE_LEN, expectedOld, target)) patched++;
    return patched;
}

static float gHalfStep = 0.0625f;

static float Nudge(float v) {
    return v + std::copysign(gHalfStep, v);
}

static SafetyHookInline& GenerateNewOneOriginal() { static SafetyHookInline h; return h; }
static SafetyHookInline& SetPosnOriginal() { static SafetyHookInline h; return h; }

static int __cdecl GenerateNewOneHook(float x, float y, float z, unsigned int modelIndex,
                                      unsigned char type, unsigned int ammo, unsigned int moneyPerDay,
                                      bool minigame, const char* message) {
    return GenerateNewOneOriginal().ccall<int>(Nudge(x), Nudge(y), Nudge(z), modelIndex, type, ammo,
                                               moneyPerDay, minigame, message);
}

static void __fastcall SetPosnHook(void* self, void*, float x, float y, float z) {
    SetPosnOriginal().thiscall<void>(self, Nudge(x), Nudge(y), Nudge(z));
}

namespace PickupFix {
    void Install() {
        float scale = Settings::world.compressedPositionScale;
        gCompress = scale;
        gDecompress = 1.0f / scale;
        gHalfStep = 0.5f / scale;

        if (Settings::world.fixPickupPositions) {
            int n = Retarget(PICKUP_DECOMPRESS_SITES, ORIGINAL_DECOMPRESS, &gDecompress)
                  + Retarget(PICKUP_COMPRESS_SITES, ORIGINAL_COMPRESS, &gCompress);
            Log::Write("pickup positions: scale %.3g (range +/-%.0f, step %.3g), %d/%u sites patched",
                       scale, 32767.0f / scale, 1.0f / scale, n,
                       (unsigned)(sizeof(PICKUP_DECOMPRESS_SITES) / sizeof(uintptr_t) + sizeof(PICKUP_COMPRESS_SITES) / sizeof(uintptr_t)));

            GenerateNewOneOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x456F20),
                                                                reinterpret_cast<void*>(&GenerateNewOneHook));
            SetPosnOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x454960),
                                                         reinterpret_cast<void*>(&SetPosnHook));
            if (GenerateNewOneOriginal() && SetPosnOriginal())
                Log::Write("pickup positions: rounded to the nearest step rather than truncated");
            else
                Log::Write("pickup positions: the rounding hooks could not be placed; a pickup may sit "
                           "up to %.3g nearer the origin than asked", 1.0f / scale);
        }
        if (Settings::world.fixCarGeneratorPositions) {
            int n = Retarget(CARGEN_DECOMPRESS_SITES, ORIGINAL_DECOMPRESS, &gDecompress)
                  + Retarget(CARGEN_COMPRESS_SITES, ORIGINAL_COMPRESS, &gCompress);
            Log::Write("car generator positions: %d/%u sites patched", n,
                       (unsigned)(sizeof(CARGEN_DECOMPRESS_SITES) / sizeof(uintptr_t) + sizeof(CARGEN_COMPRESS_SITES) / sizeof(uintptr_t)));
        }
    }
}
