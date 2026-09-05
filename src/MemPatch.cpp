#include <plugin.h>
#include <cstring>
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

    bool RetargetCall(uintptr_t callSite, uint32_t expectedTarget, const void* newTarget) {
        uint8_t opcode = 0;
        int32_t rel = 0;
        patch::GetRaw(callSite, &opcode, sizeof(opcode));
        patch::GetRaw(callSite + 1, &rel, sizeof(rel));
        uint32_t current = (uint32_t)(callSite + 5 + rel);
        if (opcode != 0xE8 || current != expectedTarget) {
            Log::Write("call patch skipped at 0x%X: found %02X -> 0x%X, expected E8 -> 0x%X "
                       "(already patched by another mod?)",
                       (unsigned)callSite, opcode, current, expectedTarget);
            return false;
        }
        patch::SetInt(callSite + 1, (uintptr_t)((intptr_t)newTarget - (intptr_t)(callSite + 5)));
        return true;
    }

    bool ReplaceBytes(uintptr_t address, const uint8_t* expected, const uint8_t* replacement, size_t length) {
        uint8_t current[16] = {};
        if (length > sizeof(current)) {
            Log::Write("patch skipped at 0x%X: %u bytes exceeds the verify buffer", (unsigned)address, (unsigned)length);
            return false;
        }
        patch::GetRaw(address, current, length);
        if (std::memcmp(current, expected, length) != 0) {
            Log::Write("patch skipped at 0x%X: bytes are %02X %02X..., expected %02X %02X... "
                       "(already patched by another mod?)",
                       (unsigned)address, current[0], current[1], expected[0], expected[1]);
            return false;
        }
        patch::SetRaw(address, (void*)replacement, length);
        return true;
    }
}
