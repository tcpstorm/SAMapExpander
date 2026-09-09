#pragma once

namespace WorldSectors {
    // Widens what San Andreas' static world grids cover, so a region placed beyond +/-3000 is
    // filed in the sector it really sits in. Only worth installing when this plugin is the one
    // placing the region's entities.
    void Install();

    // True when Install fitted the grids and this plugin is the one whose constants they read.
    // False if it left them alone - everything already inside +/-3000, a site that did not hold
    // what it should, or another mod having got there first. Nothing may move the grid unless
    // this is true.
    bool Owns();

    // What the grids currently cover, on both axes.
    void CurrentFit(float& lo, float& hi);

    // Re-fit the 120 x 120 and 30 x 30 grids to lo..hi. Cheap - the conversion sites already read
    // our globals, so this is six stores and seven immediates rewritten inside CEntity::Add and
    // CEntity::Remove - but it is only half an operation: every entity filed under the old fit
    // sits in a cell computed from the old constants, and after this those cells are somewhere
    // else. Everything must be removed from the sector lists before calling this and added back
    // after, which is what MapWindow does and the only thing that should call it.
    void Refit(float lo, float hi);
}
