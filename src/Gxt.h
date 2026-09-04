#pragma once

namespace Gxt {
    // Loads the GXT files listed in [World] Gxt and installs the CText::Get hook.
    void Install();

    // Adds one string under a GXT key (at most 7 characters, upper-cased like the game does).
    // `source` names where it came from and is only used for logging. Returns false if the key was
    // unusable. Installs the hook on first use, so it works before or after Install().
    bool AddString(const char* key, const char* text, const char* source);
}
