#pragma once

namespace Gxt {
    // Loads the GXT files listed in [World] Gxt and installs the CText::Get hook.
    void Install();

    // Adds one string under a GXT key (at most 7 characters, upper-cased like the game does).
    // `source` names where it came from and is only used for logging. Returns false if the key was
    // unusable. Installs the hook on first use, so it works before or after Install().
    bool AddString(const char* key, const char* text, const char* source);

    // True if one of our loaded files or an earlier AddString already provides this key.
    bool Has(const char* key);

    // Our text for the key, or nullptr if we have none (does not consult the game's GXT).
    const char* Find(const char* key);

    // Call once other mods (SA-MP) have finished patching: probes CText::Get with a key only we
    // provide and re-hooks on top of whatever is there now if our hook no longer answers.
    void EnsureHookIntact();
}
