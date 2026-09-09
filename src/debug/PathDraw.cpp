// The path-graph overlay: San Andreas' own line renderer, pointed at our own graphs.
//
// WHERE IT DRAWS. `Idle` opens the camera, calls RenderScene (0x53DF40), then RenderEffects
// (0x53E170), then Render2dStuff (0x53E230), then closes it. RenderEffects is the last 3D thing in
// the frame, so its call site - the `call 0x53E170` at 0x53EAD3 - is the seam: by the time it
// returns, every building, vehicle and particle is in the depth buffer, the camera transform is
// still current, and the HUD has not been drawn over the top yet. Retargeting that one call is the
// same technique WaterRender and RadarOverlay use, and it has the same virtue: MemPatch::
// RetargetCall reads the site first and refuses if another mod got there before us, so we never
// corrupt somebody else's hook.
//
// WHAT IT DRAWS WITH. CLines::RenderLineWithClipping (0x6FF4F0). Checked in the executable, not
// assumed: it tessellates the segment, clips it, fills the shared immediate-mode vertex buffer at
// 0xC4D958 and issues RwIm3DTransform / RwIm3DRenderIndexedPrimitive(rwPRIMTYPELINELIST) /
// RwIm3DEnd itself. Two consequences.
//
//   1. It sets NO render states. The only thing it calls beforehand is LittleTest (0x541330),
//      which in this HOODLUM build is a jump to 0x409B56 and does nothing but increment a counter
//      at 0xB73708. So the states are whatever the last thing to draw left behind, and this file
//      has to establish - and put back - every one it depends on.
//   2. It is one draw call per line. That is why there is a budget below rather than a loop over
//      all 8408 of Vice City's links.
//
// It also refills that shared buffer from index 0 on every call, which is the reason for drawing
// AFTER RenderEffects has returned rather than in the middle of the frame: by then every system
// that was accumulating into it - the water most of all, which flushes its own at 0x6F0064 - has
// finished with it. Drawing from a seam inside somebody else's batch would throw their geometry
// away and blame it on them.
//
// The alternative was a hand-rolled RwIm3D batch, one draw call for the lot. It was not taken:
// CLines does its own near-plane clipping, and a line that passes behind the camera is exactly the
// line whose two ends are a car and its target - the one drawing that most has to be right.
#include <plugin.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CMessages.h>
#include <cmath>
#include <cstdint>
#include <windows.h>
#include "PathDraw.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"
#include "RegionTraffic.h"
#include "RegionPeds.h"
#include "RegionParkedCars.h"

namespace {

constexpr uintptr_t kRenderEffects = 0x53E170;       // the last 3D pass of the frame
constexpr uintptr_t kRenderEffectsCall = 0x53EAD3;   // Idle's call to it, inside the camera update
constexpr uintptr_t kRenderLineWithClipping = 0x6FF4F0;

// How far from the CAMERA, not the player: the whole point of the overlay is to fly up and look
// down at a district, and culling to the player would empty the screen the moment you did.
constexpr float kRadius = 150.0f;

// One draw call per line, so this is a frame-rate ceiling and not a memory one. 1500 lines is
// about six blocks of Vice City carriageway with both lane sides drawn, which is more than can be
// read at once anyway. When it bites, the log says so rather than the picture silently truncating.
constexpr int kMaxLines = 1500;

typedef void(__cdecl* VoidFn)();
typedef void(__cdecl* LineFn)(float, float, float, float, float, float, uint32_t, uint32_t);

struct Layers {
    bool carGraph = false;
    bool pedGraph = false;
    bool drivers = false;
    bool bays = false;
    bool ground = false;
    bool xray = true;    // on by default: a graph buried under the tarmac is invisible otherwise,
                         // and "I can see nothing at all" is the least useful first look there is
};

Layers& On() { static Layers l; return l; }

int gDrawn = 0;         // lines issued this frame
bool gClipped = false;  // did the budget bite this frame
unsigned gNextReport = 0;
bool gInFrame = false;  // are we inside the render hook, i.e. is drawing legal at all

// Install() runs during static initialisation, before file-scope objects elsewhere are
// constructed - the same reason DebugKeys and RenderGuard keep their hook objects in
// function-local statics.
VoidFn& Original() { static VoidFn fn = nullptr; return fn; }

// ------------------------------------------------------------------ render state

// Saved so the HUD, which draws next, gets back the states it was promised. Only the ones this
// file changes are saved; RwRenderStateGet is cheap (it reads the device shadow, not the device).
struct SavedState {
    void* raster;
    void* vertexAlpha;
    void* srcBlend;
    void* dstBlend;
    void* zWrite;
    void* zTest;
    void* cull;
    void* shade;
    void* fog;
};

void Push(SavedState& s) {
    RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &s.raster);
    RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &s.vertexAlpha);
    RwRenderStateGet(rwRENDERSTATESRCBLEND, &s.srcBlend);
    RwRenderStateGet(rwRENDERSTATEDESTBLEND, &s.dstBlend);
    RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &s.zWrite);
    RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &s.zTest);
    RwRenderStateGet(rwRENDERSTATECULLMODE, &s.cull);
    RwRenderStateGet(rwRENDERSTATESHADEMODE, &s.shade);
    RwRenderStateGet(rwRENDERSTATEFOGENABLE, &s.fog);

    // Untextured, alpha-blended, gouraud so the two ends of a line can be different colours.
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
    RwRenderStateSet(rwRENDERSTATESHADEMODE, (void*)rwSHADEMODEGOURAUD);
    // Fog off, or a line 150 units away comes out the colour of the sky and says nothing.
    RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
    // Never write depth: a debug line must not occlude the world it is being compared against.
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    // Depth TEST is the interesting one, and it is the x-ray toggle. Off (the default) draws the
    // graph through the ground, which is the only way to see a graph that is buried. On makes the
    // road hide it, which is how you find out that it IS buried.
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(On().xray ? FALSE : TRUE));
}

void Pop(const SavedState& s) {
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, s.raster);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, s.vertexAlpha);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, s.srcBlend);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, s.dstBlend);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, s.zWrite);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, s.zTest);
    RwRenderStateSet(rwRENDERSTATECULLMODE, s.cull);
    RwRenderStateSet(rwRENDERSTATESHADEMODE, s.shade);
    RwRenderStateSet(rwRENDERSTATEFOGENABLE, s.fog);
}

// ------------------------------------------------------------------ keys

bool Edge(int vk, bool& was) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !was;
    was = down;
    return pressed;
}

void Say(const char* what, bool state) {
    // STATIC, not a local. tMessage holds a char* and not a copy of the string (CMessages.h:24),
    // so a buffer on this stack frame would be dangling by the time Render2dStuff prints it later
    // in the same frame. One shared buffer is enough: a second press only replaces the text of a
    // message that is on its way out anyway.
    static char msg[96];
    snprintf(msg, sizeof(msg), "%s %s", what, state ? "on" : "off");
    Log::Write("path overlay: %s", msg);
    CMessages::AddMessageJumpQ(msg, 2500, 0, false);
}

void PollKeys() {
    static bool wasF2 = false, wasF3 = false, wasF4 = false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    Layers& l = On();

    if (Edge(VK_F2, wasF2)) {
        if (ctrl) { l.bays = !l.bays; Say("parked-car bays", l.bays); }
        else      { l.carGraph = !l.carGraph; Say("carriageway graph", l.carGraph); }
    }
    if (Edge(VK_F3, wasF3)) {
        // The probes hang off the carriageway nodes, so they are drawn by the F2 layer and say so:
        // "ground probes on" with F2 off would otherwise be a toggle that lights nothing.
        if (ctrl) { l.ground = !l.ground; Say("ground probes (needs F2)", l.ground); }
        else      { l.pedGraph = !l.pedGraph; Say("pedestrian graph", l.pedGraph); }
    }
    if (Edge(VK_F4, wasF4)) {
        if (ctrl) { l.xray = !l.xray; Say("x-ray (draw through the world)", l.xray); }
        else      { l.drivers = !l.drivers; Say("cars being driven", l.drivers); }
    }
}

// ------------------------------------------------------------------ the frame

void Frame() {
    if (!Settings::world.debug) return;
    PollKeys();

    const Layers& l = On();
    if (!l.carGraph && !l.pedGraph && !l.drivers && !l.bays) return;

    const CVector* eye = TheCamera.GetGameCamPosition();
    if (!eye) return;

    SavedState saved;
    Push(saved);
    gDrawn = 0;
    gClipped = false;
    gInFrame = true;

    // Order matters only for readability: the graph underneath, the cars on top of it, so a car's
    // target line is the thing your eye lands on.
    if (l.carGraph) RegionTraffic::DrawGraph(*eye, kRadius, l.ground);
    if (l.pedGraph) RegionPeds::DrawGraph(*eye, kRadius);
    if (l.bays)     RegionParkedCars::DrawBays(*eye, kRadius);
    if (l.drivers)  RegionTraffic::DrawDrivers(*eye, kRadius);

    gInFrame = false;
    Pop(saved);

    if (CTimer::m_snTimeInMilliseconds >= gNextReport) {
        gNextReport = CTimer::m_snTimeInMilliseconds + 1000;
        Log::Debug("path overlay: %d line(s) within %.0f units of the camera%s", gDrawn, kRadius,
                   gClipped ? " - BUDGET REACHED, the picture is truncated. The cull is a fixed "
                              "radius, so moving will not help: turn a layer off" : "");
    }
}

void __cdecl RenderEffectsThunk() {
    Original()();
    Frame();
}

}   // namespace

namespace PathDraw {

bool Budget() { return gInFrame && gDrawn < kMaxLines; }

void Line(const CVector& a, const CVector& b, unsigned colourA, unsigned colourB) {
    if (!gInFrame) return;
    if (gDrawn >= kMaxLines) { gClipped = true; return; }
    gDrawn++;
    reinterpret_cast<LineFn>(kRenderLineWithClipping)(a.x, a.y, a.z, b.x, b.y, b.z,
                                                      (uint32_t)colourA, (uint32_t)colourB);
}

void Line(const CVector& a, const CVector& b, unsigned colour) { Line(a, b, colour, colour); }

void Cross(const CVector& at, float size, unsigned colour) {
    Line(CVector(at.x - size, at.y, at.z), CVector(at.x + size, at.y, at.z), colour);
    Line(CVector(at.x, at.y - size, at.z), CVector(at.x, at.y + size, at.z), colour);
    Line(CVector(at.x, at.y, at.z - size), CVector(at.x, at.y, at.z + size), colour);
}

void Arrow(const CVector& at, float heading, float length, unsigned colour) {
    // The game's heading convention: 0 faces +y, and it turns anticlockwise seen from above.
    const float fx = -sinf(heading), fy = cosf(heading);
    const CVector tip(at.x + fx * length, at.y + fy * length, at.z);
    Line(at, tip, colour);
    const float back = length * 0.35f, side = length * 0.22f;
    Line(tip, CVector(tip.x - fx * back - fy * side, tip.y - fy * back + fx * side, tip.z), colour);
    Line(tip, CVector(tip.x - fx * back + fy * side, tip.y - fy * back - fx * side, tip.z), colour);
}

void Install() {
    if (!Settings::world.debug) return;
    if (Settings::regions.empty()) return;

    // Under SA-MP none of the three graph modules loads its .dat at all - the server owns the
    // vehicles and the pedestrians - so there would be nothing to draw. Said out loud, because a
    // key that does nothing and says nothing is worse than a key that is not there.
    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the path overlay is off, because the traffic, pedestrian and parked-car "
                   "graphs are not loaded either");
        return;
    }

    Original() = reinterpret_cast<VoidFn>(kRenderEffects);
    if (!MemPatch::RetargetCall(kRenderEffectsCall, kRenderEffects, (const void*)&RenderEffectsThunk)) {
        Log::Write("path overlay: could not splice Idle's call to RenderEffects at 0x%X; the "
                   "F2/F3/F4 path overlay is unavailable", (unsigned)kRenderEffectsCall);
        return;
    }

    Log::Write("path overlay (debug lines, %.0f units around the CAMERA - fly up to see a district):",
               kRadius);
    Log::Write("  F2       carriageway graph: nodes, links, and every lane point a node can hand a "
               "car. A link is coloured by its own length against the arrival distance - red under "
               "2.5 (a car arrives the instant it is aimed down it), orange under 8, yellow under "
               "12 (shorter than the minimum leg), green above");
    Log::Write("  F3       pedestrian graph: a different file and a different derivation, so it is "
               "a separate answer to the same question");
    Log::Write("  F4       every car this module is driving: where it is, where the GAME says it is "
               "steering (read from the vehicle at +0x3EC), the line between the two, and the link "
               "the module believes it is on");
    Log::Write("  Ctrl+F2  parked-car bays, with the heading each was parked at");
    Log::Write("  Ctrl+F3  ground probes, drawn on the F2 layer and needing it: from each nearby "
               "carriageway node down to the surface Ground::Under finds there - green when the "
               "node is on it, magenta when it is not, a spike down when there is nothing at all");
    Log::Write("  Ctrl+F4  x-ray on/off. On (the default) draws through the world; off lets the "
               "road hide a line, which is how you find a graph buried under the tarmac");
}

}   // namespace PathDraw
