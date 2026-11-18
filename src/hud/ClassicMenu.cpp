#include <plugin.h>
#include <windows.h>
#include <CMenuManager.h>
#include <CFont.h>
#include <CSprite2d.h>
#include <CFileLoader.h>
#include <safetyhook.hpp>
#include <cstring>
#include <string>
#include <vector>
#include "ClassicMenu.h"
#include "ClassicFont.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

// ---------------------------------------------------------------------------------------------
// How a menu is reskinned without being rewritten.
//
// CMenuManager::DrawFrontEnd (0x57C290) is the whole pause menu - and the title menu, which is
// the same code - and everything it puts on screen goes through a very small number of doors:
//
//   * a black rectangle over the whole screen and one piece of page art, drawn by
//     CMenuManager::DrawBackground before anything else;
//   * text, every line of it through CFont::PrintString (0x71A700), measured where it has to be
//     with CFont::GetStringWidth (0x71A0E0) and put on screen in batches by
//     CFont::RenderFontBuffer (0x719840);
//   * the mouse pointer, one sprite in CMenuManager's own table.
//
// So for the length of that one call:
//
//   * the page is repainted with the donor's background and logo at the entry of whichever of
//     DrawStandardMenus (0x5794A0), PrintMap (0x575130) or DrawControllerSetupScreen (0x57F300)
//     DrawBackground calls first - after its black rectangle and its art, which are thereby
//     covered, and before a single entry is drawn;
//   * PrintString is answered here: the text is laid out with the donor's font, recoloured, and
//     queued, and RenderFontBuffer draws the queue - at the moments San Andreas would have drawn
//     its own, so a line still lands on top of the box that was drawn for it;
//   * GetStringWidth answers in the donor's widths, so whatever the menu positions by measuring
//     is positioned for the text that will actually appear;
//   * the pointer sprite is lent the donor's texture.
//
// Outside that call every one of those hooks forwards untouched, so the HUD, the subtitles and
// the loading screens are exactly as they were.
//
// What this does not do: move anything. Headers, entries, sliders and columns are where San
// Andreas puts them; Vice City's own layout - title at the top right, the sliding highlight bar -
// is a different piece of work. Colour tokens inside a string (the brief's, the statistics') are
// dropped rather than honoured, so those pages are one colour. Sliders and list boxes are
// rectangles San Andreas colours inline, and keep its blue, which happens to be Vice City's too.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr uintptr_t kDrawFrontEnd        = 0x57C290;
constexpr uintptr_t kDrawStandardMenus   = 0x5794A0;
constexpr uintptr_t kPrintMap            = 0x575130;
constexpr uintptr_t kDrawControllerSetup = 0x57F300;
constexpr uintptr_t kPrintString         = 0x71A700;
constexpr uintptr_t kGetStringWidth      = 0x71A0E0;
// CFont::GetStringWidth must NOT be hooked at its entry, and this is why. In the 1.0 US HOODLUM
// executable the function begins `nop; jmp 0x4016B9` - a stolen-bytes stub - and the protection
// reads the dword at 0x71A0E2, the middle of that jump, as a rotation count when it decodes a
// pointer (0x40641C: mov ecx,[0x71A0E2]; rol ebp,cl). A five-byte hook there changes the count,
// and the pointer that comes out wrong is the texture-find callback used while a vehicle model
// loads: the game dies in CGame::Init2 looking up `vehiclegrunge256`, nowhere near a font. The
// stub itself cannot take the hook either - other code jumps into the middle of it. So the menu's
// own five calls are pointed at the thunk instead, which is all that ever needed to change.
constexpr uintptr_t kStringWidthCalls[] = {
    0x57A49B,             // CMenuManager::DrawStandardMenus
    0x57FB52, 0x57FE35,   // CMenuManager::DrawControllerSetupScreen
    0x5814A7, 0x581512,   // the menu's helper text
};
constexpr uintptr_t kRenderFontBuffer    = 0x719840;

SafetyHookInline& FrontEndHook()        { static SafetyHookInline h; return h; }
SafetyHookInline& StandardMenusHook()   { static SafetyHookInline h; return h; }
SafetyHookInline& PrintMapHook()        { static SafetyHookInline h; return h; }
SafetyHookInline& ControllerSetupHook() { static SafetyHookInline h; return h; }
SafetyHookInline& PrintStringHook()     { static SafetyHookInline h; return h; }
SafetyHookInline& RenderBufferHook()    { static SafetyHookInline h; return h; }

enum { kPageMap = 5, kPageControls = 38 };

// ------------------------------------------------------------------------------------- assets

enum { kUntried = 0, kLoaded = 1, kFailed = -1 };
int gState = kUntried;
RwTexture* gBackground = nullptr;
RwTexture* gLogo = nullptr;
RwTexture* gPointer = nullptr;

RwTexDictionary* LoadDictionary(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    return CFileLoader::LoadTexDictionary(path.c_str());
}

RwTexture* FindTexture(RwTexDictionary* dictionary, const char* name) {
    RwTexture* texture = dictionary ? RwTexDictionaryFindNamedTexture(dictionary, name) : nullptr;
    if (texture) {
        RwTextureSetFilterMode(texture, rwFILTERLINEAR);
        RwTextureSetAddressing(texture, rwTEXTUREADDRESSCLAMP);
    }
    return texture;
}

bool Ready() {
    if (gState != kUntried) return gState == kLoaded;
    gState = kFailed;

    const std::string& folder = Settings::world.hudAssets;
    // The HUD may have loaded the fonts already; if the HUD is San Andreas' they are still ours
    // to load, since the menu's text is drawn with them either way.
    if (!ClassicFont::Ready()) {
        RwTexDictionary* fonts = LoadDictionary(folder + "\\fonts.txd");
        ClassicFont::SetTextures(FindTexture(fonts, "font1"), FindTexture(fonts, "font2"));
    }
    RwTexDictionary* menu = LoadDictionary(folder + "\\fronten1.txd");
    gBackground = FindTexture(menu, "background");
    gLogo = FindTexture(menu, "vc_logo");
    gPointer = FindTexture(menu, "mouse");

    if (!ClassicFont::Ready() || !gBackground) {
        Log::Write("menu: MenuStyle is set, but %s could not be loaded from %s, so San Andreas' own "
                   "menu is drawn. tools\\import-city.ps1 <donor game> <San Andreas> -HudOnly writes "
                   "it; a folder imported before the menu was supported has no fronten1.txd",
                   !gBackground ? "fronten1.txd" : "fonts.txd", folder.c_str());
        return false;
    }
    gState = kLoaded;
    Log::Write("menu: Vice City style, drawn from %s%s%s", folder.c_str(),
               gLogo ? "" : " (no vc_logo in fronten1.txd, so no logo)",
               gPointer ? "" : " (no mouse in fronten1.txd, so San Andreas' pointer)");
    return true;
}

// -------------------------------------------------------------------------------------- drawing

float ScreenW() { return (float)RsGlobal.maximumWidth; }
float ScreenH() { return (float)RsGlobal.maximumHeight; }

void DrawSprite(RwTexture* texture, float left, float top, float right, float bottom) {
    float posn[8] = { left, top, right, top, right, bottom, left, bottom };
    float uv[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, texture->raster);
    CSprite2d::SetVertices(4, posn, uv, CRGBA(255, 255, 255, 255));
    RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, 4);
}

bool gInMenu = false;
bool gBackgroundPending = false;

// The donor's background over the whole screen, and its logo. Vice City hangs the logo in the top
// left, which is where San Andreas writes the page title, so it goes in the top right instead -
// kept square, a little over a quarter of the screen high - and not at all on the two pages that
// use the whole top edge.
void PaintBackground(CMenuManager* menu) {
    if (!gBackgroundPending) return;
    gBackgroundPending = false;

    const float w = ScreenW(), h = ScreenH();
    DrawSprite(gBackground, 0.0f, 0.0f, w, h);
    const int page = menu->m_nCurrentMenuPage;
    if (gLogo && page != kPageMap && page != kPageControls) {
        const float size = h * (130.0f / 480.0f);
        const float right = w - h * (27.0f / 480.0f), top = h * (8.0f / 480.0f);
        DrawSprite(gLogo, right - size, top, right, top + size);
    }
}

// ----------------------------------------------------------------------------------------- text

struct Line {
    ClassicFont::Format format;
    float x, y;
    std::string text;
};
std::vector<Line> gQueue;

void Flush() {
    for (const Line& line : gQueue) ClassicFont::Print(line.format, line.x, line.y, line.text.c_str());
    gQueue.clear();
}

// San Andreas' menu colours onto Vice City's. Everything the menu writes is one of a handful of
// constants, so they are matched exactly and anything else - white, black, the error red - is
// left as it was asked for. Alpha always carries over, because the menu fades with it.
CRGBA Recolour(const CRGBA& c) {
    struct Pair { unsigned char r, g, b, R, G, B; };
    static const Pair kPairs[] = {
        { 172, 203, 241, 255, 150, 225 },   // selected entry, page title -> Vice City's pink
        {  74,  90, 107, 195,  90, 165 },   // entry not selected        -> its darker pink
        {  14,  30,  47, 110,  50,  95 },   // entry that cannot be used -> darker again
        { 225, 225, 225, 255, 190, 235 },   // body text                 -> a pale pink
    };
    for (const Pair& p : kPairs)
        if (c.r == p.r && c.g == p.g && c.b == p.b) return CRGBA(p.R, p.G, p.B, c.a);
    return c;
}

// CFont's state as the menu left it, turned into the donor font's terms. The scale carries over
// as it is: both games draw a character into a 32-wide cell and half a 40-high one times the
// scale, because San Andreas' font code is Vice City's.
ClassicFont::Format CurrentFormat() {
    ClassicFont::Format f;
    // San Andreas' gothic title and its menu face both become the heavy face, which is what Vice
    // City writes its titles and its entries in; the subtitle face becomes the plain one.
    f.style = CFont::m_FontStyle == FONT_SUBTITLES ? ClassicFont::Standard : ClassicFont::Heading;
    f.scaleX = CFont::m_Scale->x;
    f.scaleY = CFont::m_Scale->y;
    f.proportional = CFont::m_bFontPropOn;
    f.align = CFont::m_bFontCentreAlign ? ClassicFont::Centre
            : CFont::m_bFontRightAlign ? ClassicFont::Right : ClassicFont::Left;
    f.color = Recolour(*CFont::m_Color);
    // San Andreas outlines its menu text; the donor fonts were drawn to carry a drop shadow, so
    // an edge of n becomes a shadow of n.
    const unsigned char edge = CFont::m_nFontOutlineSize > CFont::m_nFontShadow
                             ? CFont::m_nFontOutlineSize : CFont::m_nFontShadow;
    f.shadowX = f.shadowY = (float)edge * ScreenH() / 448.0f;
    f.dropColor = CRGBA(CFont::m_FontDropColor->r, CFont::m_FontDropColor->g, CFont::m_FontDropColor->b, f.color.a);
    return f;
}

bool IsNewline(const char* s) { return s[0] == '~' && (s[1] == 'n' || s[1] == 'N') && s[2] == '~'; }

// One PrintString call, broken into lines the way CFont would break it - at ~n~, and at a space
// when the next word would pass the limit the alignment implies - and queued a line at a time.
void QueueText(float x, float y, const char* text) {
    ClassicFont::Format f = CurrentFormat();
    float limit = f.align == ClassicFont::Centre ? CFont::m_fFontCentreSize
                : f.align == ClassicFont::Right ? x - CFont::m_fRightJustifyWrap
                : CFont::m_fWrapx - x;
    if (limit <= 0.0f) limit = 1.0e9f;
    const float step = 18.0f * f.scaleY;
    const float space = ClassicFont::Width(f, " ");

    std::string line, word;
    float lineWidth = 0.0f;
    auto endLine = [&]() {
        if (!line.empty()) gQueue.push_back({ f, x, y, line });
        y += step;
        line.clear();
        lineWidth = 0.0f;
    };
    auto endWord = [&]() {
        if (word.empty()) return;
        const float width = ClassicFont::Width(f, word.c_str());
        if (!line.empty() && lineWidth + space + width > limit) endLine();
        if (!line.empty()) { line += ' '; lineWidth += space; }
        line += word;
        lineWidth += width;
        word.clear();
    };

    for (const char* s = text; *s;) {
        if (IsNewline(s)) { endWord(); endLine(); s += 3; }
        else if (*s == ' ') { endWord(); s++; }
        else if (*s == '~') {
            // Any other token travels with its word; the font skips it when measuring and drawing.
            const char* close = strchr(s + 1, '~');
            const char* end = close ? close + 1 : s + strlen(s);
            word.append(s, end);
            s = end;
        }
        else word += *s++;
    }
    endWord();
    if (!line.empty()) gQueue.push_back({ f, x, y, line });
}

// ---------------------------------------------------------------------------------------- hooks

void __fastcall FrontEndThunk(CMenuManager* menu, void*) {
    if (!Ready()) {
        FrontEndHook().thiscall<void>(menu);
        return;
    }
    CSprite2d& pointer = menu->FrontEndSprites[MENUSPRITE_MOUSE];
    RwTexture* own = pointer.m_pTexture;
    // Only once San Andreas has loaded its own: a null here means "not loaded yet", and the menu
    // frees whatever is in the slot when it unloads.
    if (gPointer && own) pointer.m_pTexture = gPointer;
    gInMenu = true;
    gBackgroundPending = true;
    FrontEndHook().thiscall<void>(menu);
    Flush();
    gInMenu = false;
    gBackgroundPending = false;
    if (pointer.m_pTexture == gPointer) pointer.m_pTexture = own;
}

void __fastcall StandardMenusThunk(CMenuManager* menu, void*, bool drawTitle) {
    if (gInMenu) PaintBackground(menu);
    StandardMenusHook().thiscall<void>(menu, drawTitle);
}

void __fastcall PrintMapThunk(CMenuManager* menu, void*) {
    if (gInMenu) PaintBackground(menu);
    PrintMapHook().thiscall<void>(menu);
}

void __fastcall ControllerSetupThunk(CMenuManager* menu, void*) {
    if (gInMenu) PaintBackground(menu);
    ControllerSetupHook().thiscall<void>(menu);
}

void __cdecl PrintStringThunk(float x, float y, const char* text) {
    if (gInMenu && text) QueueText(x, y, text);
    else PrintStringHook().ccall<void>(x, y, text);
}

float __cdecl StringWidthThunk(const char* text, bool spaces, bool scriptValues) {
    if (!gInMenu || !text)
        return reinterpret_cast<float(__cdecl*)(const char*, bool, bool)>(kGetStringWidth)(text, spaces, scriptValues);
    // Without `spaces` San Andreas measures one word, which is how its own wrapping asks.
    if (!spaces) {
        const char* end = strchr(text, ' ');
        if (end) return ClassicFont::Width(CurrentFormat(), std::string(text, end).c_str());
    }
    return ClassicFont::Width(CurrentFormat(), text);
}

void __cdecl RenderBufferThunk() {
    RenderBufferHook().ccall<void>();
    if (gInMenu) Flush();
}

bool Place(SafetyHookInline& hook, uintptr_t address, void* thunk, const char* name) {
    hook = safetyhook::create_inline(reinterpret_cast<void*>(address), thunk);
    if (!hook) Log::Write("menu: the hook on %s (0x%X) could not be placed", name, (unsigned)address);
    return (bool)hook;
}

}   // namespace

namespace ClassicMenu {
    void Install() {
        const int style = Settings::world.menuStyle;
        if (style == 0) return;
        if (style == 2) {
            Log::Write("menu: MenuStyle = III is not drawn yet, so San Andreas' own menu is used");
            return;
        }

        // The page hooks first and the wrapper last: with the wrapper missing nothing else ever
        // switches on, which is the safe way round for a partial install.
        int placed = 0;
        placed += Place(StandardMenusHook(), kDrawStandardMenus, reinterpret_cast<void*>(&StandardMenusThunk), "CMenuManager::DrawStandardMenus");
        placed += Place(PrintMapHook(), kPrintMap, reinterpret_cast<void*>(&PrintMapThunk), "CMenuManager::PrintMap");
        placed += Place(ControllerSetupHook(), kDrawControllerSetup, reinterpret_cast<void*>(&ControllerSetupThunk), "CMenuManager::DrawControllerSetupScreen");
        placed += Place(PrintStringHook(), kPrintString, reinterpret_cast<void*>(&PrintStringThunk), "CFont::PrintString");
        // Not fatal if one of these is refused: that line is measured in San Andreas' widths and
        // sits a little off, which is a blemish and not a reason to give up the menu.
        int measured = 0;
        for (uintptr_t site : kStringWidthCalls)
            measured += MemPatch::RetargetCall(site, kGetStringWidth, reinterpret_cast<const void*>(&StringWidthThunk));
        placed += Place(RenderBufferHook(), kRenderFontBuffer, reinterpret_cast<void*>(&RenderBufferThunk), "CFont::RenderFontBuffer");
        if (placed == 5)
            placed += Place(FrontEndHook(), kDrawFrontEnd, reinterpret_cast<void*>(&FrontEndThunk), "CMenuManager::DrawFrontEnd");
        Log::Write("menu: Vice City style requested, %d of 6 hooks placed%s; %d of %d width calls "
                   "repointed", placed, placed == 6 ? "" : " - without all of them the menu is left San "
                   "Andreas'", measured, (int)(sizeof(kStringWidthCalls) / sizeof(kStringWidthCalls[0])));
    }
}
