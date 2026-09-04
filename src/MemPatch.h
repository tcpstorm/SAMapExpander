#pragma once
#include <cstdint>

namespace MemPatch {
    // Rewrites the disp32 operand of an instruction like `fmul dword ptr [imm32]` so it reads
    // from `newTarget` instead. Refuses (and logs) if the current operand isn't `expectedOld`.
    bool RetargetDisp32(uintptr_t instruction, unsigned opcodeLength, uint32_t expectedOld, const void* newTarget);

    // Points an existing `call rel32` at `newTarget`. Refuses (and logs) unless the site really is a
    // near call to `expectedTarget`, which also catches another mod having hooked the same site.
    bool RetargetCall(uintptr_t callSite, uint32_t expectedTarget, const void* newTarget);
}
