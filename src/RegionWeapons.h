#pragma once
#include <cstdint>

// The guns a region's own game had, given to San Andreas over the weapon types it already knows.
namespace RegionWeapons {
    void Install();

    // Where CModelInfo::ms_weaponModelInfoStore lives now, and how many CWeaponModelInfo it holds:
    // the vanilla 51 - which San Andreas' own fifty weapons all but fill - or the heap block that
    // replaced it, so a region's weapon models have somewhere to go. CityLoader budgets `weap` IDE
    // lines against these, exactly as it does tobj lines against ModelLimits' time store.
    // True once Install() has decided a region's guns really are going in: a Weapons file that
    // reads, RegionWeapons on, not SA-MP, and the weapon model store moved out of the way. Until
    // then a region's weap IDE lines have nowhere to go and must not be offered to the game.
    bool Active();

    uintptr_t Store();
    int Capacity();

    // The game's own tables, read rather than called: CWeaponInfo::FindWeaponType answers 0 for a
    // name it does not know, which is UNARMED, so anything that has to tell "no such weapon" from
    // "unarmed" has to do the lookup itself. -1 for an unknown name, -1 for a weapon with no model.
    int WeaponType(const char* name);
    int WeaponModel(int weaponType);
}
