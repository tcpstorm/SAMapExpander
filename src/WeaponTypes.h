#pragma once
#include <cstdint>

// Widens San Andreas' weapon system so that a gun can be ADDED to it rather than written over one
// of its own. See WeaponTypes.cpp for what is patched and why it is safe.
namespace WeaponTypes {
    // Verify and widen. Must run before CWeaponInfo::LoadWeaponData, because that is what looks a
    // weapon name up, and before anything can be fired. Answers whether new types may be handed
    // out; when it answers false the game is left exactly as San Andreas wrote it.
    bool Install();
    bool Available();

    // Claim the next free weapon type for a gun that should behave like `donorType` - the San
    // Andreas weapon whose case in every switch on weapon type it will share. `name` is what
    // weapon.dat and the script opcodes will call it, and must outlive the process. -1 when there
    // is no free type left, or when Install() declined.
    int Add(const char* name, int donorType);

    // The San Andreas weapon a new type borrows its code paths from, or -1 for anything else.
    int DonorOf(int type);

    // Remember where a new weapon's model info lives, which the caller has just looked up by name.
    // Two of the game's functions read CWeaponModelInfo::m_nWeaponType and compare it against a
    // fixed list, and this is how they are answered for; the array those pointers usually come from
    // has been moved by ModelLimits, so the pointer has to be handed over rather than computed.
    void SetModelInfo(int type, void* modelInfo);

    // True when aWeaponInfo[type] is still the record CWeaponInfo::Initialise left behind - a slot
    // of -1, which no weapon.dat line has overwritten. Only meaningful once the weapon table has
    // loaded, which is why claiming a type and writing its record are two separate steps.
    bool RecordIsFree(int type);

    // The range of type numbers this hands out, and how many are left.
    int FirstType();
    int LastType();
    int Remaining();
}
