#include <plugin.h>
#include <CSprite2d.h>
#include "ClassicFont.h"

// Advance widths, in atlas pixels of a 32-pixel cell, for Vice City's two atlases. Index is the
// glyph's cell number; 208 is the width of a space in the heavy face and 209 the fixed advance
// used when proportional spacing is off. These are the tables CFont::Size holds in Vice City
// (English/French/Italian/German/Spanish set), as documented by the reVC project.
static const short kSize[2][210] = {
    {   // font2
        12,  9, 22, 17, 19, 19, 25,  4, 33, 33, 25, 35, 11, 10,  6, 33,
        18, 10, 17, 17, 17, 17, 17, 15, 12, 16,  5, 30, 30, 30, 30, 30,
        12, 16, 19, 16, 19, 18, 18, 17, 22, 11, 17, 18, 18, 30, 22, 19,
        22, 19, 19, 20, 18, 19, 19, 29, 19, 18, 19, 19, 33, 33, 10, 19,
        12, 14, 11, 11, 16, 11, 12, 14, 14, 10, 13, 12, 10, 19, 18, 12,
        16, 13, 13, 11, 12, 15, 12, 15, 13, 12, 12, 37, 33, 37, 35, 37,
        16, 16, 16, 16, 33, 17, 18, 18, 18, 18, 11, 11, 11, 11, 19, 19,
        19, 19, 19, 19, 19, 19, 15, 14, 14, 14, 14, 20, 14, 11, 11, 11,
        11, 10, 10, 10, 10, 12, 12, 12, 12, 15, 15, 15, 15, 22, 18, 21,
        10, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19, 16
    },
    {   // font1: the plain face in cells 0..143, the heavy one from 144
        15,  7, 31, 25, 20, 23, 21,  7, 11, 10, 26, 14,  6, 12,  6, 26,
        20,  7, 20, 20, 21, 20, 20, 19, 21, 20,  8, 30, 24, 30, 24, 19,
        20, 22, 22, 21, 22, 18, 18, 22, 22,  9, 14, 21, 18, 27, 21, 24,
        22, 22, 23, 20, 19, 23, 22, 31, 23, 23, 21, 25, 13, 30,  7, 19,
        10, 17, 17, 16, 17, 17, 11, 17, 17,  7,  7, 18,  7, 25, 17, 17,
        17, 17, 11, 17, 11, 17, 18, 25, 19, 18, 17, 28, 26, 20, 15, 15,
        20, 20, 20, 20, 29, 22, 19, 19, 19, 19,  9,  9,  9,  9, 23, 23,
        23, 23, 24, 24, 24, 24, 20, 19, 17, 17, 17, 30, 16, 17, 17, 17,
        17, 11, 11, 15, 12, 17, 17, 17, 17, 17, 17, 17, 17, 21, 17, 19,
        20, 18, 19, 19, 21, 19, 19, 19, 19, 19, 16, 19, 19, 19, 20, 19,
        16, 19, 19,  9, 19, 20, 14, 29, 19, 19, 19, 19, 19, 19, 21, 19,
        20, 32, 20, 19, 19, 19, 19, 19, 19, 29, 19, 19, 19, 19, 19,  9,
         9,  9,  9, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 10,  9,
        10, 20
    }
};

// The same for GTA III, whose atlases are laid out the same way and hold other faces: font2 is
// the leaning sans it writes names and body text in, font1 the heavy face of its numbers and
// menus - with the heart, the shield and the star among its glyphs - and there is no second face
// in font1's lower half. 192 is the fixed advance. These are CFont::Size as GTA III's own
// executable holds it (the third table, the pager's, is not used here).
static const short kSizeIII[2][193] = {
    {   // font2
        13, 12, 31, 35, 23, 35, 31,  9, 14, 15, 25, 30, 11, 17, 13, 31,
        23, 16, 22, 21, 24, 23, 23, 20, 23, 22, 10, 35, 26, 26, 26, 26,
        30, 26, 24, 23, 24, 22, 21, 24, 26, 10, 20, 26, 22, 29, 26, 25,
        23, 25, 24, 24, 22, 25, 24, 29, 29, 23, 25, 37, 22, 37, 35, 37,
        35, 21, 22, 21, 21, 22, 13, 22, 21, 10, 16, 22, 11, 32, 21, 21,
        23, 22, 16, 20, 14, 21, 20, 30, 25, 21, 21, 33, 33, 33, 33, 35,
        27, 27, 27, 27, 32, 24, 23, 23, 23, 23, 11, 11, 11, 11, 26, 26,
        26, 26, 26, 26, 26, 25, 26, 21, 21, 21, 21, 32, 23, 22, 22, 22,
        22, 11, 11, 11, 11, 22, 22, 22, 22, 22, 22, 22, 22, 26, 21, 24,
        12, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
        26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 18, 26, 26,
        26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
        20
    },
    {   // font1
        15, 14, 16, 25, 19, 26, 22, 11, 18, 18, 27, 26, 13, 19,  9, 27,
        19, 18, 19, 19, 22, 19, 20, 18, 19, 20, 12, 32, 15, 32, 15, 35,
        15, 19, 19, 19, 19, 19, 16, 19, 20,  9, 19, 20, 14, 29, 19, 20,
        19, 19, 19, 19, 21, 19, 20, 32, 20, 19, 19, 33, 31, 39, 37, 39,
        37, 21, 21, 21, 23, 21, 19, 23, 23, 10, 19, 20, 16, 26, 23, 23,
        20, 20, 20, 22, 21, 22, 22, 26, 22, 22, 23, 35, 35, 35, 35, 37,
        19, 19, 19, 19, 29, 19, 19, 19, 19, 19,  9,  9,  9,  9, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 30, 19, 19, 19, 19,
        19, 10, 10, 10, 10, 19, 19, 19, 19, 19, 19, 19, 19, 19, 23, 35,
        12, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 11, 19, 19,
        19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
        19
    }
};

static ClassicFont::Game gGame = ClassicFont::ViceCity;

static RwTexture* gFont1 = nullptr;
static RwTexture* gFont2 = nullptr;

// The heavy face is not a font of its own: it is the lower half of font1, and a character is
// moved down into it by this table. Anything it does not move - the heart at 91, the shield at
// 28 - is deliberately left in the plain half, which is how the health line gets its icon.
static unsigned HeavyCell(unsigned c) {
    if (c >= 16 && c <= 26) return c + 128;   // 0-9 :
    if (c >= 8 && c <= 9) return c + 86;      // ( )
    if (c == 4) return c + 89;                // $
    if (c == 7) return 206;                   // '
    if (c == 14) return 207;                  // .
    if (c >= 33 && c <= 58) return c + 122;   // A-Z
    if (c >= 65 && c <= 90) return c + 90;    // a-z, drawn as capitals
    if (c >= 96 && c <= 118) return c + 85;
    if (c >= 119 && c <= 140) return c + 62;
    if (c >= 141 && c <= 142) return 204;
    if (c == 143) return 205;
    if (c == 1) return 208;                   // !
    return c;
}

static unsigned Cell(ClassicFont::Style style, unsigned char ch) {
    unsigned c = ch < 32 ? 0 : ch - 32u;
    if (gGame == ClassicFont::LibertyCity) return c > 191 ? 0 : c;
    if (style == ClassicFont::Heading) return HeavyCell(c);
    return c > 155 ? 0 : c;
}

static float Advance(const ClassicFont::Format& f, unsigned cell) {
    if (gGame == ClassicFont::LibertyCity)
        return f.scaleX * (float)kSizeIII[f.style == ClassicFont::Heading ? 1 : 0][f.proportional ? cell : 192];
    const short* table = kSize[f.style == ClassicFont::Bank ? 0 : 1];
    return f.scaleX * (float)table[f.proportional ? cell : 209];
}

// "~r~" and its kin are San Andreas colour and button tokens. None of them means anything to
// this renderer, and printing the tilde would put a stray glyph in a zone name.
static const char* SkipToken(const char* s) {
    const char* close = s + 1;
    while (*close && *close != '~') close++;
    return *close ? close + 1 : close;
}

static void Run(const ClassicFont::Format& f, float x, float y, const char* text, const CRGBA& color,
                float slantRefX, float slantRefY) {
    // GTA III has one face to an atlas; Vice City keeps two of its three in font1.
    const bool iii = gGame == ClassicFont::LibertyCity;
    RwTexture* texture = (iii ? f.style != ClassicFont::Heading : f.style == ClassicFont::Bank) ? gFont2 : gFont1;
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, texture->raster);

    for (const char* s = text; *s;) {
        if (*s == '~') { s = SkipToken(s); continue; }
        const unsigned cell = Cell(f.style, (unsigned char)*s++);
        // Cell 0 is a space - and, in font1, where the heavy face keeps its exclamation mark,
        // which is why a space is skipped by what it was asked for rather than by where it lands.
        const bool blank = cell == 0;
        const unsigned drawn = (!iii && f.style == ClassicFont::Heading && cell == 208) ? 0 : cell;

        if (!blank) {
            const float top = f.slant != 0.0f ? (slantRefX - x) * f.slant + slantRefY : y;
            const float col = (float)(drawn % 16), row = (float)(drawn / 16);
            const float u0 = col / 16.0f, u1 = (col + 1.0f) / 16.0f - 0.001f;
            const float v = row / 12.8f, vNext = (row + 1.0f) / 12.8f;
            float height = 40.0f * f.scaleY * 0.5f;
            float uv[8];
            if (f.slant != 0.0f && !iii) {
                // The quad stays upright; the texture is sampled on a lean instead. Vice City
                // only: GTA III's slanted line is upright characters on a climbing baseline.
                uv[0] = u0; uv[1] = v + 0.00055f;
                uv[2] = u1; uv[3] = v + 0.0021f + 0.01f;
                uv[4] = u1; uv[5] = vNext - 0.0021f + 0.01f;
                uv[6] = u0; uv[7] = vNext - 0.009f;
            } else if (drawn < 192 || iii) {
                uv[0] = u0; uv[1] = v + 0.0021f;
                uv[2] = u1; uv[3] = v + 0.0021f;
                uv[4] = u1; uv[5] = vNext - 0.0021f;
                uv[6] = u0; uv[7] = vNext - 0.0021f;
            } else {
                // The last rows of the atlas are cut short, so the cell is too.
                height = 33.0f * f.scaleY * 0.5f;
                uv[0] = u0; uv[1] = v + 0.0021f;
                uv[2] = u1; uv[3] = v + 0.0021f;
                uv[4] = u1; uv[5] = vNext - 0.017f;
                uv[6] = u0; uv[7] = vNext - 0.017f;
            }
            const float right = x + 32.0f * f.scaleX;
            float posn[8] = { x, top, right, top, right, top + height, x, top + height };
            CSprite2d::SetVertices(4, posn, uv, color);
            RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, 4);
        }
        x += Advance(f, cell);
    }
}

namespace ClassicFont {
    void SetGame(Game game) { gGame = game; }

    void SetTextures(RwTexture* font1, RwTexture* font2) {
        gFont1 = font1;
        gFont2 = font2;
    }

    bool Ready() { return gFont1 && gFont2; }

    float Width(const Format& f, const char* text) {
        float w = 0.0f;
        if (!text) return w;
        for (const char* s = text; *s;) {
            if (*s == '~') { s = SkipToken(s); continue; }
            w += Advance(f, Cell(f.style, (unsigned char)*s++));
        }
        return w;
    }

    void Print(const Format& f, float x, float y, const char* text) {
        if (!text || !*text || !Ready()) return;

        const float width = Width(f, text);
        const float left = f.align == Right ? x - width : f.align == Centre ? x - width * 0.5f : x;

        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
        RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);

        if (f.shadowX != 0.0f || f.shadowY != 0.0f)
            Run(f, left + f.shadowX, y + f.shadowY, text, f.dropColor,
                f.slantRefX + f.shadowX, f.slantRefY + f.shadowY);
        Run(f, left, y, text, f.color, f.slantRefX, f.slantRefY);
    }
}
