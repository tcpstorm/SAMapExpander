#pragma once

namespace Gxt {
    // Loads the GXT files listed in [World] Gxt and hooks CText::Load so our strings are merged into
    // the game's own key table every time it is (re)loaded.
    void Install();

    // Adds one string under a GXT key (at most 7 characters, upper-cased like the game does).
    // `source` names where it came from and is only used for logging. Returns false if the key was
    // unusable. Works before or after Install(); a string added after the game's table was built is
    // merged in on the next frame.
    bool AddString(const char* key, const char* text, const char* source);

    // True if one of our loaded files or an earlier AddString already provides this key.
    bool Has(const char* key);

    // Our text for the key, or nullptr if we have none (does not consult the game's GXT).
    const char* Find(const char* key);

    // Once per frame: re-merges our strings if the game's table was rebuilt without us noticing
    // (a text reload from a hook another mod broke, or a mod that swapped the block).
    void EnsureInjected();
}
