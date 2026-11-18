#pragma once
#include <RenderWare.h>
#include <CRGBA.h>

// The font of GTA III's and Vice City's HUD, drawn out of the donor game's own fonts.txd.
//
// San Andreas cannot be asked to do this. CFont has four styles over two atlases of its own and a
// glyph table per style compiled into the executable; pointing it at another game's atlas gives
// that game's shapes at San Andreas' widths, which is every character in the wrong place. So this
// is a second, much smaller text renderer that knows the donor's layout: sixteen 32x40 cells to a
// row, the donor's own advance widths, and its three styles over two textures.
namespace ClassicFont {
    enum Style {
        Bank,       // font2: the handwritten face of the zone and vehicle names
        Standard,   // font1, upper half: the plain face, and the heart, shield and star glyphs
        Heading     // font1, lower half: the heavy face of the money, the clock and the health
    };
    enum Align { Left, Centre, Right };

    struct Format {
        Style style = Standard;
        float scaleX = 1.0f, scaleY = 1.0f;   // in pixels: the caller has applied the screen scale
        bool  proportional = true;            // off = every character advances by the same width
        Align align = Left;
        CRGBA color = CRGBA(255, 255, 255, 255);
        float shadowX = 0.0f, shadowY = 0.0f; // pixel offset of the drop shadow; both 0 for none
        CRGBA dropColor = CRGBA(0, 0, 0, 255);
        // A slanted line climbs to the right: each character sits at
        // (slantRefX - x) * slant + slantRefY, and the y handed to Print is not used.
        float slant = 0.0f, slantRefX = 0.0f, slantRefY = 0.0f;
    };

    // font1 and font2 out of the donor's fonts.txd. Either may be null, and then nothing draws.
    void SetTextures(RwTexture* font1, RwTexture* font2);
    bool Ready();

    float Width(const Format& format, const char* text);
    void Print(const Format& format, float x, float y, const char* text);
}
