#include <plugin.h>
#include "MemPatch.h"
#include "Log.h"

using namespace plugin;

namespace MemPatch {
    bool RetargetDisp32(uintptr_t instruction, unsigned opcodeLength, uint32_t expectedOld, const void* newTarget) {
        uintptr_t operand = instruction + opcodeLength;
        uint32_t current = 0;
        patch::GetRaw(operand, &current, sizeof(current));
        if (current != expectedOld) {
            Log::Write("patch skipped at 0x%X: operand is 0x%X, expected 0x%X (already patched by another mod?)",
                       (unsigned)instruction, current, expectedOld);
            return false;
        }
        patch::SetUInt(operand, (uintptr_t)newTarget);
        return true;
    }
}
