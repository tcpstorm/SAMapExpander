#include <plugin.h>
#include <windows.h>
#include <CHud.h>
#include <CFont.h>
#include <CSprite2d.h>
#include <CRadar.h>
#include <CMenuManager.h>
#include <CFileLoader.h>
#include <CTxdStore.h>
#include <CModelInfo.h>
#include <CKeyGen.h>
#include <CWeaponInfo.h>
#include <CPlayerPed.h>
#include <CPlayerInfo.h>
#include <CPlayerData.h>
#include <CPedIntelligence.h>
#include <CWanted.h>
#include <CWorld.h>
#include <CClock.h>
#include <CTimer.h>
#include <CStats.h>
#include <CDarkel.h>
#include <CCamera.h>
#include <CTheScripts.h>
#include <CEntryExitManager.h>
#include <safetyhook.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include "ClassicHud.h"
#include "ClassicFont.h"
#include "Settings.h"
#include "Zones.h"
#include <CGame.h>
#include "Log.h"

using namespace plugin;

// ---------------------------------------------------------------------------------------------
// What this replaces, and how.
//
// San Andreas draws its HUD from a handful of free functions that CHud::Draw (0x58FAE0) and
// CHud::DrawAfterFade (0x58D490) call one after another, and each is nothing but drawing: no game
// state is decided in any of them, only the fade timers of the thing being drawn. So each can be
// answered whole, at its entry, without the rest of the game noticing:
//
//   CHud::DrawPlayerInfo   0x58EAF0   clock, money, weapon icon and ammo, health, armour, breath
//   CHud::DrawWanted       0x58D9A0   the stars - drawn by the replacement above, so this is empty
//   CHud::DrawAreaName     0x58AA50   the zone name
//   CHud::DrawVehicleName  0x58AEA0   the vehicle name
//   CHud::DrawRadar        0x58A330   left to the game, except for the ring round the disc
//
// The layout, the colours and the fonts are Vice City's, as the reVC project documents them
// (CHud::Draw in its src/renderer/Hud.cpp): everything is right-aligned against the weapon icon
// in a 640x448 frame. The art is the donor game's own - hud.txd, fonts.txd and the weapon icons
// out of its archive - converted by tools/import_city.py, because San Andreas' raster reader
// refuses a Direct3D 8 texture outright.
//
// GTA III's HUD is the same block with other numbers - Vice City's was made from it - so it is
// drawn by the same code: its own colours, the heavy face a little larger, the ammunition written
// in black across the foot of the weapon icon, the stars as glyphs of the heavy face with their
// shadows always showing, the names smaller and at the very bottom, and a plain black ring. Its
// layout is CHud::Draw as the re3 project documents it; its weapon icons are in hud.txd itself.
//
// What stays San Andreas': the radar itself and where it sits, the blips, subtitles, help boxes,
// mission timers and the stats panel. The radar is not moved down to where Vice City had it: its
// position is a set of constants shared with every blip and marker transform, and with the
// widescreen fixes that patch them.
//
// Screen scale. San Andreas multiplies a 640x448 coordinate by the screen size and by the two
// floats at 0x859520 (1/640) and 0x859524 (1/448). The widescreen fixes work by rewriting those
// two floats, so reading them - rather than dividing by 640 and 448 here - is what keeps this
// HUD the same shape as the radar it is drawn beside, whichever of them is installed.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr uintptr_t kDrawPlayerInfo  = 0x58EAF0;
constexpr uintptr_t kDrawWanted      = 0x58D9A0;
constexpr uintptr_t kDrawAreaName    = 0x58AA50;
constexpr uintptr_t kDrawVehicleName = 0x58AEA0;
constexpr uintptr_t kDrawRadar       = 0x58A330;
constexpr uintptr_t kDrawBlips       = 0x588050;   // CRadar::DrawBlips

SafetyHookInline& PlayerInfoHook()  { static SafetyHookInline h; return h; }
SafetyHookInline& WantedHook()      { static SafetyHookInline h; return h; }
SafetyHookInline& AreaNameHook()    { static SafetyHookInline h; return h; }
SafetyHookInline& VehicleNameHook() { static SafetyHookInline h; return h; }
SafetyHookInline& RadarHook()       { static SafetyHookInline h; return h; }
SafetyHookInline& BlipsHook()       { static SafetyHookInline h; return h; }

// Whose HUD: 1 Vice City's, 2 GTA III's. Set once, at install.
int gStyle = 1;
bool LibertyCity() { return gStyle == 2; }
const char* StyleName() { return LibertyCity() ? "GTA III" : "Vice City"; }

// Each game's palette, and the few numbers of the layout that differ between them.
struct Look {
    CRGBA money, ammo, health, armour, wanted, wantedFlash, notWanted, zone, vehicle, clock;
    float numberScaleX, numberScaleY;   // the heavy face of the clock, the money and the health
    const char* armourGlyph;            // the shield: a different character in each game's atlas
    float nameScaleX, nameScaleY, nameShadow;
    float zoneFromBottom, vehicleFromBottom;
    float ringGrow;                     // how far the ring stands out from the radar's own box
};
const Look kLooks[2] = {
    {   // Vice City
        CRGBA(0, 207, 133, 255), CRGBA(255, 150, 225, 255), CRGBA(255, 150, 225, 255),
        CRGBA(185, 185, 185, 255), CRGBA(97, 194, 247, 255), CRGBA(62, 141, 181, 255),
        CRGBA(27, 89, 130, 255), CRGBA(45, 155, 90, 255), CRGBA(97, 194, 247, 255),
        CRGBA(97, 194, 247, 255),
        0.7f, 1.25f, "<", 1.7f, 1.8f, 2.0f, 128.0f, 105.0f, 6.0f
    },
    {   // GTA III
        CRGBA(89, 115, 150, 255), CRGBA(0, 0, 0, 255), CRGBA(186, 101, 50, 255),
        CRGBA(124, 140, 95, 255), CRGBA(193, 164, 120, 255), CRGBA(193, 164, 120, 255),
        CRGBA(0, 0, 0, 255), CRGBA(152, 154, 82, 255), CRGBA(194, 165, 120, 255),
        CRGBA(194, 165, 120, 255),
        0.8f, 1.35f, "[", 1.2f, 1.2f, 1.0f, 30.0f, 55.0f, 4.0f
    }
};
const Look& L() { return kLooks[LibertyCity() ? 1 : 0]; }
const CRGBA kDebt(180, 25, 29, 255);        // San Andreas' own red: neither game ever showed a debt

// CHud::m_ItemToFlash, which the mission script uses to point at part of the HUD.
enum { kFlashArmour = 3, kFlashHealth = 4 };
// The name fade state machine shared by the zone and the vehicle name.
enum { kNameHidden = 0, kNameShown = 1, kNameFadeIn = 2, kNameFadeOut = 3, kNameSwitch = 4 };

// ------------------------------------------------------------------------------------- assets

enum { kUntried = 0, kLoaded = 1, kFailed = -1 };
int gState = kUntried;
RwTexDictionary* gWeaponIcons = nullptr;   // optional: the donor's own weapon icons, by name
RwTexture* gFist = nullptr;
RwTexture* gRadarDisc = nullptr;
RwTexture* gBlank = nullptr;               // stands in for San Andreas' ring while the radar draws

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

// A small, fully transparent texture. San Andreas draws its radar ring as four mirrored quarters
// of one sprite in the middle of CHud::DrawRadar; lending that sprite this for the length of the
// call is how the ring is taken away without touching the function.
RwTexture* MakeBlankTexture() {
    RwImage* image = RwImageCreate(4, 4, 32);
    if (!image) return nullptr;
    if (!RwImageAllocatePixels(image)) { RwImageDestroy(image); return nullptr; }
    memset(image->cpPixels, 0, (size_t)image->stride * 4);

    RwTexture* texture = nullptr;
    RwInt32 width = 0, height = 0, depth = 0, format = 0;
    if (RwImageFindRasterFormat(image, rwRASTERTYPETEXTURE, &width, &height, &depth, &format)) {
        if (RwRaster* raster = RwRasterCreate(width, height, depth, format)) {
            if (RwRasterSetFromImage(raster, image)) texture = RwTextureCreate(raster);
            if (!texture) RwRasterDestroy(raster);
        }
    }
    RwImageFreePixels(image);
    RwImageDestroy(image);
    return texture;
}

// On the first frame that draws a HUD, because that is the first moment RenderWare is certain to
// be up; and once only, because a missing file does not appear half way through a session.
bool Ready() {
    if (gState != kUntried) return gState == kLoaded;
    gState = kFailed;

    const std::string& folder = Settings::world.hudAssets;
    RwTexDictionary* hud = LoadDictionary(folder + "\\hud.txd");
    RwTexDictionary* fonts = LoadDictionary(folder + "\\fonts.txd");
    RwTexture* font1 = FindTexture(fonts, "font1");
    RwTexture* font2 = FindTexture(fonts, "font2");
    gFist = FindTexture(hud, "fist");
    gRadarDisc = FindTexture(hud, "radardisc");

    if (!font1 || !font2 || !gFist || !gRadarDisc) {
        Log::Write("hud: HudStyle is set, but %s%s%s could not be loaded from %s, so San Andreas' own "
                   "HUD is drawn. tools\\import-city.ps1 <donor game> <San Andreas> -HudOnly writes "
                   "them; the donor game's own files cannot be used as they are, because San "
                   "Andreas does not read a Direct3D 8 texture",
                   (!font1 || !font2) ? "fonts.txd" : "",
                   ((!font1 || !font2) && (!gFist || !gRadarDisc)) ? " and " : "",
                   (!gFist || !gRadarDisc) ? "hud.txd" : "", folder.c_str());
        return false;
    }

    ClassicFont::SetGame(LibertyCity() ? ClassicFont::LibertyCity : ClassicFont::ViceCity);
    ClassicFont::SetTextures(font1, font2);
    // Vice City keeps each icon in its weapon's own dictionary, gathered by the importer into
    // weapons.txd; GTA III keeps them all beside the fist.
    gWeaponIcons = LibertyCity() ? hud : LoadDictionary(folder + "\\weapons.txd");
    gBlank = MakeBlankTexture();
    gState = kLoaded;
    Log::Write("hud: %s style, drawn from %s%s%s", StyleName(), folder.c_str(),
               gWeaponIcons ? "" : " (no weapons.txd there, so every weapon but the fist keeps "
                                   "its San Andreas icon)",
               gBlank ? "" : " (the blank texture could not be made, so the radar keeps San "
                             "Andreas' ring)");
    return true;
}

// -------------------------------------------------------------------------------------- screen

float ScreenW() { return (float)RsGlobal.maximumWidth; }
float ScreenH() { return (float)RsGlobal.maximumHeight; }
// The game's own horizontal scale: what the radar is drawn with, so what its ring must use.
float GameScaleX(float a) { return a * ScreenW() * *reinterpret_cast<const float*>(0x859520); }
// Everything else. Unpatched, San Andreas stretches a 4:3 HUD across whatever the screen is, and
// Vice City's fixed-width numbers and square weapon box show that far more than San Andreas' own
// bars do. So on a screen wider than 4:3, with the game's scale still the vanilla 1/640 - that is,
// with no widescreen fix installed to have already dealt with it - the width follows the height
// instead and the block keeps its shape against the right edge - see [World] HudKeepAspect.
float ScaleX(float a) {
    const float game = *reinterpret_cast<const float*>(0x859520);
    const float w = ScreenW(), h = ScreenH();
    const bool vanilla = game > 0.0015624f && game < 0.0015626f;
    // Auto follows the Widescreen switch in the game's own display options: on, the world is
    // drawn in the screen's real proportions and the HUD should be too; off, the game is a 4:3
    // picture stretched to fit, and a HUD that was not stretched with it would be the odd one out.
    const int mode = Settings::world.hudKeepAspect;
    const bool keep = mode == 1 || (mode == 2 && FrontEndMenuManager.m_bPrefsUseWideScreen);
    if (keep && vanilla && w * 3.0f > h * 4.0f) return a * h / 480.0f;
    return a * w * game;
}
float ScaleY(float a) { return a * ScreenH() * *reinterpret_cast<const float*>(0x859524); }
float FromRight(float a) { return ScreenW() - ScaleX(a); }
float FromBottom(float a) { return ScreenH() - ScaleY(a); }

float StepMs() { return CTimer::ms_fTimeStep * 20.0f; }   // a time step of 1.0 is a fiftieth of a second

CRGBA WithAlpha(const CRGBA& c, float alpha) {
    return CRGBA(c.r, c.g, c.b, (unsigned char)(alpha < 0.0f ? 0.0f : alpha > 255.0f ? 255.0f : alpha));
}

void DrawSprite(RwTexture* texture, float left, float top, float right, float bottom,
                const CRGBA& color, float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f) {
    float posn[8] = { left, top, right, top, right, bottom, left, bottom };
    float uv[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, texture->raster);
    CSprite2d::SetVertices(4, posn, uv, color);
    RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, 4);
}

// The heavy face at the size every number on the HUD uses, right-aligned with a two-unit shadow.
ClassicFont::Format NumberFormat(const CRGBA& color) {
    ClassicFont::Format f;
    f.style = ClassicFont::Heading;
    f.scaleX = ScaleX(L().numberScaleX);
    f.scaleY = ScaleY(L().numberScaleY);
    f.proportional = false;
    f.align = ClassicFont::Right;
    f.color = color;
    f.shadowX = ScaleX(2.0f);
    f.shadowY = ScaleY(2.0f);
    f.dropColor = CRGBA(0, 0, 0, color.a);
    return f;
}

// --------------------------------------------------------------------------------- weapon icon

// The Vice City weapon whose icon stands for a San Andreas weapon type: the same gun where both
// games have it, and nothing where San Andreas' has no counterpart - a spray can keeps its own
// icon rather than borrowing somebody else's.
const char* DonorIconNameIII(int weaponType) {
    switch (weaponType) {
    case 5:  return "bat";
    case 16: return "grenade";
    case 18: return "molotov";
    case 22: return "pistol";
    case 25: return "shotgun";
    case 28: return "uzi";
    case 30: return "ak47";
    case 31: return "m16";
    case 34: return "sniper";
    case 35:
    case 36: return "rocket";
    case 37: return "flame";
    case 40: return "detonator";
    default: return nullptr;
    }
}

const char* DonorIconName(int weaponType) {
    if (LibertyCity()) return DonorIconNameIII(weaponType);
    switch (weaponType) {
    case 1:  return "brassknuckle";
    case 2:  return "golfclub";
    case 3:  return "nitestick";
    case 4:  return "knifecur";
    case 5:  return "bat";
    case 8:  return "katana";
    case 9:  return "chnsaw";
    case 16: return "grenade";
    case 17: return "teargas";
    case 18: return "molotov";
    case 22: return "colt45";
    case 24: return "python";
    case 25: return "chromegun";
    case 26: return "buddyshot";
    case 27: return "shotgspa";
    case 28: return "uzi";
    case 29: return "mp5lng";
    case 31: return "m4";
    case 32: return "tec9";
    case 34: return "sniper";
    case 35:
    case 36: return "rocketla";
    case 37: return "flame";
    case 38: return "minigun";
    case 39:
    case 40: return "bomb";
    case 43: return "camera";
    default: return nullptr;
    }
}

// What San Andreas itself would show: "<model name>icon" out of the weapon model's dictionary,
// found by key the way CHud::DrawWeaponIcon (0x58D7D0) finds it. For a gun a region imported this
// is already the donor game's art, since the importer writes it there under that name.
RwTexture* OwnIcon(int modelId) {
    CBaseModelInfo* info = CModelInfo::ms_modelInfoPtrs[modelId];
    if (!info) return nullptr;
    TxdDef* dictionary = CTxdStore::ms_pTxdPool->GetAt(info->m_nTxdIndex);
    if (!dictionary || !dictionary->m_pRwDictionary) return nullptr;
    return RwTexDictionaryFindHashNamedTexture(dictionary->m_pRwDictionary,
                                               CKeyGen::AppendStringToKey(info->m_nKey, "ICON"));
}

void DrawWeapon(CPlayerPed* ped) {
    const CWeapon& weapon = ped->m_aWeapons[ped->m_nSelectedWepSlot];
    const int type = (int)weapon.m_eWeaponType;
    CWeaponInfo* base = CWeaponInfo::GetWeaponInfo(weapon.m_eWeaponType, 1);
    if (!base) return;

    RwTexture* icon = nullptr;
    if (base->m_nModelId <= 0) {
        icon = gFist;
    } else {
        if (const char* donor = DonorIconName(type)) icon = FindTexture(gWeaponIcons, donor);
        if (!icon) icon = OwnIcon(base->m_nModelId);
    }
    if (icon)
        DrawSprite(icon, FromRight(99.0f), ScaleY(27.0f), FromRight(35.0f), ScaleY(91.0f),
                   CRGBA(255, 255, 255, 255), 0.015f, 0.015f, 1.0f, 1.0f);

    // Whether a count is shown at all is San Andreas' rule (CHud::DrawAmmo, 0x5893B0), because it
    // is San Andreas' weapons being counted: nothing for a melee weapon, a gift, a parachute or
    // anything fired by being used.
    const unsigned total = weapon.m_nAmmoTotal, inClip = weapon.m_nAmmoInClip;
    if (total - inClip >= 9999 || CDarkel::FrenzyOnGoing() || base->m_nSlot <= 1 ||
        base->m_nWeaponFire == WEAPON_FIRE_USE || type == 40 || type == 46 ||
        (type >= 10 && type <= 15))
        return;

    CWeaponInfo* skilled = CWeaponInfo::GetWeaponInfo(weapon.m_eWeaponType, ped->GetWeaponSkill());
    const unsigned clip = skilled ? skilled->m_nAmmoClip : base->m_nAmmoClip;
    char text[32];
    if (clip <= 1 || clip >= 1000) {
        snprintf(text, sizeof(text), "%u", total);
    } else {
        const unsigned divisor = type == 37 ? 10 : 1;   // the flamethrower counts in tenths
        unsigned spare = (total - inClip) / divisor;
        if (spare > 9999) spare = 9999;
        snprintf(text, sizeof(text), "%u-%u", spare, inClip / divisor);
    }

    ClassicFont::Format f;
    f.align = ClassicFont::Centre;
    f.color = L().ammo;
    if (LibertyCity()) {
        // Small and black, across the foot of the icon rather than under it.
        f.style = ClassicFont::Bank;
        f.scaleX = ScaleX(0.4f);
        f.scaleY = ScaleY(0.6f);
        ClassicFont::Print(f, FromRight(66.0f), ScaleY(73.0f), text);
        return;
    }
    f.style = ClassicFont::Standard;
    f.scaleX = ScaleX(0.5f);
    f.scaleY = ScaleY(0.8f);
    f.shadowX = ScaleX(2.0f);
    f.shadowY = ScaleY(2.0f);
    ClassicFont::Print(f, FromRight(66.0f), ScaleY(90.0f), text);
}

// --------------------------------------------------------------------------- the player's block

void DrawPlayerInfoClassic() {
    CPlayerPed* ped = FindPlayerPed(-1);
    if (!ped || !ped->m_pPlayerData) return;
    const CPlayerInfo& info = CWorld::Players[(unsigned char)CWorld::PlayerInFocus];
    const unsigned now = CTimer::m_snTimeInMilliseconds;
    const unsigned frame = CTimer::m_FrameCounter;
    char text[32];

    if (CHud::bDrawClock) {
        snprintf(text, sizeof(text), "%02d:%02d", CClock::ms_nGameClockHours, CClock::ms_nGameClockMinutes);
        ClassicFont::Print(NumberFormat(L().clock), FromRight(111.0f), ScaleY(22.0f), text);
    }

    const int money = info.m_nDisplayMoney;
    if (money < 0) snprintf(text, sizeof(text), "-$%07d", -money);
    else           snprintf(text, sizeof(text), "$%08d", money);
    ClassicFont::Print(NumberFormat(money < 0 ? kDebt : L().money), FromRight(110.0f), ScaleY(43.0f), text);

    DrawWeapon(ped);

    // Health: the number, and a heart that blinks for two seconds after a hit. Below ten the
    // whole line blinks, and the script can make it blink to point at it.
    const bool blink = (frame & 8) != 0;
    if (!((CHud::m_ItemToFlash == kFlashHealth || ped->m_fHealth < 10.0f) && blink)) {
        ClassicFont::Format f = NumberFormat(L().health);
        snprintf(text, sizeof(text), "%03d", (int)(ped->m_fHealth + 0.5f));
        ClassicFont::Print(f, FromRight(110.0f), ScaleY(65.0f), text);
        if (!info.m_nLastTimeEnergyLost || now > info.m_nLastTimeEnergyLost + 2000 || (frame & 4))
            ClassicFont::Print(f, FromRight(164.0f), ScaleY(65.0f), "{");
    }

    if (ped->m_fArmour > 1.0f && !(CHud::m_ItemToFlash == kFlashArmour && blink)) {
        ClassicFont::Format f = NumberFormat(L().armour);
        snprintf(text, sizeof(text), "%03d", (int)(ped->m_fArmour + 0.5f));
        ClassicFont::Print(f, FromRight(182.0f), ScaleY(65.0f), text);
        if (!info.m_nLastTimeArmourLost || now > info.m_nLastTimeArmourLost + 2000 || (frame & 4))
            ClassicFont::Print(f, FromRight(234.0f), ScaleY(65.0f), L().armourGlyph);
    }

    // The wanted level: six stars from the right, the ones not earned drawn dark. A level that
    // has just changed blinks for two seconds, and so does one that is only suspended.
    CWanted* wanted = ped->m_pPlayerData->m_pWanted;
    if (wanted && LibertyCity()) {
        // GTA III's: the star is a glyph of the heavy face, set from its left edge, and what is
        // always there is its black shadow - the star itself only once it has been earned.
        ClassicFont::Format f = NumberFormat(L().wanted);
        f.proportional = true;
        f.align = ClassicFont::Left;
        f.shadowX = f.shadowY = 0.0f;
        for (unsigned i = 0; i < 6; i++) {
            const float x = FromRight(60.0f + 23.0f * (float)i);
            f.color = CRGBA(0, 0, 0, 255);
            ClassicFont::Print(f, x + ScaleX(2.0f), ScaleY(89.0f), "]");
            if (wanted->m_nWantedLevel > i &&
                (now > wanted->m_nLastTimeWantedLevelChanged + 2000 || (frame & 4))) {
                f.color = L().wanted;
                ClassicFont::Print(f, x, ScaleY(87.0f), "]");
            }
        }
    } else if (wanted) {
        ClassicFont::Format f = NumberFormat(L().wanted);
        f.style = ClassicFont::Standard;
        f.proportional = true;
        for (unsigned i = 0; i < 6; i++) {
            if (wanted->m_nWantedLevel > i &&
                (now > wanted->m_nLastTimeWantedLevelChanged + 2000 || (frame & 4)))
                f.color = L().wanted;
            else if (wanted->m_nWantedLevelBeforeParole > i && (frame & 4))
                f.color = L().wantedFlash;
            else if (wanted->m_nWantedLevel <= i)
                f.color = L().notWanted;
            else
                continue;
            ClassicFont::Print(f, FromRight(110.0f + 23.0f * (float)i), ScaleY(87.0f), ">");
        }
    }

    // Breath. Vice City has nothing to copy - Tommy cannot swim - so this is San Andreas' own bar
    // in Vice City's blue, under the stars, shown on San Andreas' own terms: while swimming or
    // short of breath, and for half a second after.
    const float lungs = CStats::GetFatAndMuscleModifier((eStatModAbilities)8);
    const bool swimming = ped->m_pIntelligence && ped->m_pIntelligence->GetTaskSwim();
    if (swimming || ped->m_pPlayerData->m_fBreath < lungs) CHud::m_LastBreathTime = (int)now;
    if (lungs > 0.0f && CHud::m_LastBreathTime && (unsigned)CHud::m_LastBreathTime + 500 > now) {
        float percent = ped->m_pPlayerData->m_fBreath / lungs * 100.0f;
        percent = percent < 0.0f ? 0.0f : percent > 100.0f ? 100.0f : percent;
        CSprite2d::DrawBarChart(FromRight(110.0f), ScaleY(113.0f), (unsigned short)ScaleX(62.0f),
                                (unsigned char)ScaleY(9.0f), percent, 0, 0, 1, L().clock, CRGBA(0, 0, 0, 0));
    }
}

// ----------------------------------------------------------------------- zone and vehicle names

// Both names are Vice City's: the handwritten face, leaning up to the right, against the right
// edge above where the radar's row ends.
void PrintName(const char* text, float fromBottom, const CRGBA& color, float alpha) {
    ClassicFont::Format f;
    f.style = ClassicFont::Bank;
    f.scaleX = ScaleX(L().nameScaleX);
    f.scaleY = ScaleY(L().nameScaleY);
    f.align = ClassicFont::Right;
    f.color = WithAlpha(color, alpha);
    f.shadowX = ScaleX(L().nameShadow);
    f.shadowY = ScaleY(L().nameShadow);
    f.dropColor = WithAlpha(CRGBA(0, 0, 0, 255), alpha);
    f.slant = 0.15f;
    f.slantRefX = FromRight(32.0f);
    f.slantRefY = FromBottom(fromBottom);
    ClassicFont::Print(f, f.slantRefX, f.slantRefY, text);
}

// The fade state machines below are San Andreas' own, step for step (as the gta-reversed project
// documents CHud::DrawAreaName and CHud::DrawVehicleName), because the rest of the game drives
// them: SetZoneName and SetVehicleName only set the pointers these compare. Only the last few
// lines of each - what is actually printed, and where - are Vice City's.
void DrawAreaNameClassic() {
    if (!CHud::m_pZoneName) return;

    if (CHud::m_pZoneName != CHud::m_pLastZoneName) {
        switch (CHud::m_ZoneState) {
        case kNameHidden:
            if ((!CTheScripts::bPlayerIsOffTheMap && CTheScripts::bDisplayHud) ||
                CEntryExitManager::ms_exitEnterState == 1 || CEntryExitManager::ms_exitEnterState == 2) {
                CHud::m_ZoneState = kNameFadeIn;
                CHud::m_ZoneNameTimer = 0;
                CHud::m_ZoneFadeTimer = 0;
                CHud::m_ZoneToPrint = CHud::m_pZoneName;
                if (CHud::m_VehicleState == kNameShown || CHud::m_VehicleState == kNameFadeIn)
                    CHud::m_VehicleState = kNameFadeOut;
            }
            break;
        case kNameShown:
        case kNameFadeIn:
        case kNameFadeOut:
            CHud::m_ZoneState = kNameSwitch;
            CHud::m_ZoneNameTimer = 0;
            break;
        case kNameSwitch:
            CHud::m_ZoneNameTimer = 0;
            break;
        }
        CHud::m_pLastZoneName = CHud::m_pZoneName;
    }
    if (!CHud::m_ZoneState) return;

    const bool screenFadingIn = TheCamera.GetScreenFadeStatus() == 2;
    float alpha = 255.0f;
    switch (CHud::m_ZoneState) {
    case kNameShown:
        CHud::m_ZoneFadeTimer = 1000;
        if (CHud::m_ZoneNameTimer > 3000) CHud::m_ZoneState = kNameFadeOut;
        break;
    case kNameFadeIn:
        if (!TheCamera.GetFading() && !screenFadingIn) CHud::m_ZoneFadeTimer += (int)StepMs();
        if (CHud::m_ZoneFadeTimer > 1000) {
            CHud::m_ZoneFadeTimer = 1000;
            CHud::m_ZoneState = kNameShown;
        }
        if (screenFadingIn) {
            CHud::m_ZoneState = kNameFadeOut;
            CHud::m_ZoneFadeTimer = 1000;
        } else {
            alpha = (float)CHud::m_ZoneFadeTimer / 1000.0f * 255.0f;
        }
        break;
    case kNameFadeOut:
        if (!TheCamera.GetFading() && !screenFadingIn) CHud::m_ZoneFadeTimer -= (int)StepMs();
        if (CHud::m_ZoneFadeTimer < 0) {
            CHud::m_ZoneFadeTimer = 0;
            CHud::m_ZoneState = kNameHidden;
        }
        if (screenFadingIn) CHud::m_ZoneFadeTimer = 1000;
        else alpha = (float)CHud::m_ZoneFadeTimer / 1000.0f * 255.0f;
        break;
    case kNameSwitch:
        CHud::m_ZoneFadeTimer -= (int)StepMs();
        if (CHud::m_ZoneFadeTimer < 0) {
            CHud::m_ZoneFadeTimer = 0;
            CHud::m_ZoneState = kNameFadeIn;
            CHud::m_ZoneToPrint = CHud::m_pLastZoneName;
        }
        alpha = (float)CHud::m_ZoneFadeTimer / 1000.0f * 255.0f;
        break;
    }

    // A subtitle or a big message has the bottom of the screen; the name gives way to it.
    if (CHud::m_Message[0] || BigMessageX[1] != 0.0f || BigMessageX[2] != 0.0f) {
        CHud::m_ZoneState = kNameFadeOut;
        return;
    }

    CHud::m_ZoneNameTimer += (int)StepMs();
    PrintName(CHud::m_ZoneToPrint, L().zoneFromBottom, L().zone, alpha);
}

// The zone name under SA-MP, where the code above never gets a name to show.
//
// San Andreas' own display is fed by CHud::SetZoneName, and SA-MP switches that off - its server
// call for turning area names back on was removed years ago - so m_pZoneName stays null and
// DrawAreaName has nothing to draw, however many zones the region registered. So there the name
// is found here instead, once a frame, with the same lookup the game uses
// (CTheZones::FindSmallestZoneForPosition), and shown on Vice City's own schedule as reVC
// documents it in CHud::Draw: fade in over a second, stay for ten, fade out over a second; and
// when the player crosses into another zone while one is showing, the old name fades out before
// the new one fades in. It keeps its own state rather than CHud's, so nothing SA-MP does to those
// can disturb it.
void DrawZoneNameTracked(CPlayerPed* ped) {
    static const char* shown = nullptr;     // the name being faded in, held or faded out
    static const char* last = nullptr;      // what the lookup answered last frame
    static int state = kNameHidden;
    static float fade = 0.0f, held = 0.0f;  // both in milliseconds

    // Nothing is named indoors: an interior has no zone of its own and would show the district
    // it happens to be parked under.
    const CVector& at = ped->GetPosition();
    const char* here = CGame::currArea == 0 ? Zones::DisplayNameAt(at.x, at.y, at.z) : nullptr;

    if (here && here != last) {
        if (state == kNameHidden) {
            state = kNameFadeIn;
            shown = here;
            fade = held = 0.0f;
        } else {
            state = kNameSwitch;
        }
    }
    last = here;
    if (state == kNameHidden || !shown) return;

    const float step = StepMs();
    switch (state) {
    case kNameShown:
        fade = 1000.0f;
        if (held > 10000.0f) state = kNameFadeOut;
        break;
    case kNameFadeIn:
        fade += step;
        if (fade > 1000.0f) { fade = 1000.0f; state = kNameShown; }
        break;
    case kNameFadeOut:
        fade -= step;
        if (fade < 0.0f) { fade = 0.0f; state = kNameHidden; }
        break;
    case kNameSwitch:
        fade -= step;
        if (fade < 0.0f) {
            fade = held = 0.0f;
            // Into whatever the player is standing in now, which may no longer be what started
            // the switch; into nothing at all if they have walked indoors meanwhile.
            shown = here;
            state = here ? kNameFadeIn : kNameHidden;
        }
        break;
    }

    // A subtitle or a big message has the bottom of the screen; the name gives way to it.
    if (CHud::m_Message[0] || BigMessageX[1] != 0.0f || BigMessageX[2] != 0.0f) {
        if (state != kNameHidden) state = kNameFadeOut;
        return;
    }
    held += step;
    if (state != kNameHidden && shown && !TheCamera.GetFading())
        PrintName(shown, L().zoneFromBottom, L().zone, fade * 0.001f * 255.0f);
}

void DrawVehicleNameClassic() {
    if (!CHud::m_pVehicleName) {
        CHud::m_VehicleState = kNameHidden;
        CHud::m_VehicleNameTimer = 0;
        CHud::m_VehicleFadeTimer = 0;
        CHud::m_pLastVehicleName = nullptr;
        return;
    }

    if (CHud::m_pVehicleName != CHud::m_pLastVehicleName) {
        if (CHud::m_VehicleState == kNameHidden) {
            CHud::m_VehicleState = kNameFadeIn;
            CHud::m_VehicleNameTimer = 0;
            CHud::m_VehicleFadeTimer = 0;
            CHud::m_pVehicleNameToPrint = CHud::m_pVehicleName;
            if (CHud::m_ZoneState == kNameShown || CHud::m_ZoneState == kNameFadeIn)
                CHud::m_ZoneState = kNameFadeOut;
        } else {
            CHud::m_VehicleState = kNameSwitch;
            CHud::m_VehicleNameTimer = 0;
        }
        CHud::m_pLastVehicleName = CHud::m_pVehicleName;
    }
    if (!CHud::m_VehicleState) return;

    float alpha = 0.0f;
    switch (CHud::m_VehicleState) {
    case kNameShown:
        if (CHud::m_VehicleNameTimer > 3000) {
            CHud::m_VehicleState = kNameFadeOut;
            CHud::m_VehicleFadeTimer = 1000;
        }
        alpha = 255.0f;
        break;
    case kNameFadeIn:
        CHud::m_VehicleFadeTimer += (int)StepMs();
        if (CHud::m_VehicleFadeTimer > 1000) {
            CHud::m_VehicleFadeTimer = 1000;
            CHud::m_VehicleState = kNameShown;
        }
        alpha = (float)CHud::m_VehicleFadeTimer / 1000.0f * 255.0f;
        break;
    case kNameFadeOut:
        CHud::m_VehicleFadeTimer -= (int)StepMs();
        if (CHud::m_VehicleFadeTimer < 0) {
            CHud::m_VehicleState = kNameHidden;
            CHud::m_VehicleFadeTimer = 0;
        }
        alpha = (float)CHud::m_VehicleFadeTimer / 1000.0f * 255.0f;
        break;
    case kNameSwitch:
        CHud::m_VehicleFadeTimer -= (int)StepMs();
        if (CHud::m_VehicleFadeTimer < 0) {
            CHud::m_VehicleNameTimer = 0;
            CHud::m_VehicleState = kNameFadeIn;
            CHud::m_VehicleFadeTimer = 0;
            CHud::m_pVehicleNameToPrint = CHud::m_pLastVehicleName;
        }
        alpha = (float)CHud::m_VehicleFadeTimer / 1000.0f * 255.0f;
        break;
    }

    if (CHud::m_Message[0]) return;
    CHud::m_VehicleNameTimer += (int)StepMs();
    if (CTheScripts::bDisplayHud)
        PrintName(CHud::m_pVehicleNameToPrint, L().vehicleFromBottom, L().vehicle, alpha);
}

// --------------------------------------------------------------------------------------- radar

// CHud::DrawRadar draws the map, then its ring, then calls CRadar::DrawBlips - which is also what
// the pause map calls, so the flag says which caller this is. The ring goes down between the map
// and the blips, where San Andreas' own went, so a blip pinned to the edge still sits on top.
bool gDrawingHudRadar = false;

void DrawRadarRing() {
    // The radar is 94 x 76 with its corner 40 in and 104 up; Vice City's ring is that grown by 6,
    // drawn once in black two units low and once in its own colours. GTA III's is grown by 4 and
    // drawn once, in black.
    const float grow = L().ringGrow;
    const float left = GameScaleX(40.0f - grow), right = GameScaleX(134.0f + grow);
    const float top = FromBottom(104.0f + grow), bottom = FromBottom(28.0f - grow);
    if (LibertyCity()) {
        DrawSprite(gRadarDisc, left, top, right, bottom, CRGBA(0, 0, 0, 255));
        return;
    }
    const float drop = ScaleY(2.0f);
    DrawSprite(gRadarDisc, left, top + drop, right, bottom + drop, CRGBA(0, 0, 0, 255));
    DrawSprite(gRadarDisc, left, top, right, bottom, CRGBA(255, 255, 255, 255));
}

// ---------------------------------------------------------------------------------------- hooks

// Whether the zone name is found here rather than handed over by the game - see
// DrawZoneNameTracked. Decided once, at install, because that is when it is known.
bool gTrackZoneName = false;

void __cdecl PlayerInfoThunk() {
    if (Ready()) {
        DrawPlayerInfoClassic();
        if (gTrackZoneName)
            if (CPlayerPed* ped = FindPlayerPed(-1)) DrawZoneNameTracked(ped);
    }
    else PlayerInfoHook().ccall<void>();
}

void __cdecl WantedThunk() {
    // Otherwise the stars are already down - unless the hook that draws them never took.
    if (!Ready() || !PlayerInfoHook()) WantedHook().ccall<void>();
}

void __cdecl AreaNameThunk() {
    if (Ready()) { if (!gTrackZoneName) DrawAreaNameClassic(); }
    else AreaNameHook().ccall<void>();
}

void __cdecl VehicleNameThunk() {
    if (Ready()) DrawVehicleNameClassic();
    else VehicleNameHook().ccall<void>();
}

void __cdecl RadarThunk() {
    if (!Ready() || !gBlank) {
        RadarHook().ccall<void>();
        return;
    }
    CSprite2d& ring = CHud::Sprites[HUDSPRITE_RADARDISC];
    RwTexture* own = ring.m_pTexture;
    ring.m_pTexture = gBlank;
    gDrawingHudRadar = true;
    RadarHook().ccall<void>();
    gDrawingHudRadar = false;
    ring.m_pTexture = own;
}

void __cdecl BlipsThunk() {
    // Radar mode 1 is "blips only": no map, and in either game no ring.
    if (gDrawingHudRadar && FrontEndMenuManager.m_nPrefsRadarMode != 1) DrawRadarRing();
    gDrawingHudRadar = false;
    BlipsHook().ccall<void>();
}

bool Place(SafetyHookInline& hook, uintptr_t address, void* thunk, const char* name) {
    hook = safetyhook::create_inline(reinterpret_cast<void*>(address), thunk);
    if (!hook) Log::Write("hud: the hook on %s (0x%X) could not be placed; that part keeps San "
                          "Andreas' look", name, (unsigned)address);
    return (bool)hook;
}

}   // namespace

namespace ClassicHud {
    void Install() {
        const int style = Settings::world.hudStyle;
        if (style == 0) return;
        gStyle = style;

        gTrackZoneName = Settings::world.hudZoneNames == 1 ||
                         (Settings::world.hudZoneNames == 2 && Settings::RunningUnderSaMp());
        int placed = 0;
        placed += Place(PlayerInfoHook(), kDrawPlayerInfo, reinterpret_cast<void*>(&PlayerInfoThunk), "CHud::DrawPlayerInfo");
        placed += Place(WantedHook(), kDrawWanted, reinterpret_cast<void*>(&WantedThunk), "CHud::DrawWanted");
        placed += Place(AreaNameHook(), kDrawAreaName, reinterpret_cast<void*>(&AreaNameThunk), "CHud::DrawAreaName");
        placed += Place(VehicleNameHook(), kDrawVehicleName, reinterpret_cast<void*>(&VehicleNameThunk), "CHud::DrawVehicleName");
        placed += Place(RadarHook(), kDrawRadar, reinterpret_cast<void*>(&RadarThunk), "CHud::DrawRadar");
        placed += Place(BlipsHook(), kDrawBlips, reinterpret_cast<void*>(&BlipsThunk), "CRadar::DrawBlips");
        Log::Write("hud: %s style requested, %d of 6 hooks placed; its art is loaded from %s "
                   "on the first frame that draws a HUD; zone names %s", StyleName(), placed,
                   Settings::world.hudAssets.c_str(),
                   gTrackZoneName ? "are looked up by the plugin each frame"
                                  : "are the ones the game hands to its own HUD");
    }
}
