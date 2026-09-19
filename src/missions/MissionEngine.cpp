// The bytecode interpreter that runs one mined mission at a time. See MissionEngine.h and
// docs/missions.md for the design; MissionData.h/.cpp is the file format this reads.
//
// WHY AN INTERPRETER AND NOT A FLATTENED OBJECTIVE LIST. An earlier version of this file ran a
// curated list of "spawn this, then watch these die" steps built by throwing away every
// IF/GOTO_IF_FALSE/GOTO/variable instruction in the donor script. That cannot sequence a mission
// at all - "brief, then drive to the mark, then the twist" needs the donor's own control flow,
// not a bag of things to do in whatever order they were mined. So the program IS the donor
// script's own instruction stream (tools/import_missions.py's serialize_program), and this steps
// through it exactly as the game's own script VM would: an instruction pointer, a WAIT that
// yields the current frame, IF/GOTO_IF_FALSE branching on real conditions evaluated against the
// live game state, and global/local integer and float variables.
//
// WHAT IS NOT REBUILT. Two things, both explained in full where they are handled below:
//   - EXTERNAL JUMPS. A large fraction of the donor script's own GOTO/GOTO_IF_FALSE targets - the
//     tool's own log names how many per mission - land on a handful of addresses just before each
//     mission's own mined range: a shared subroutine (almost certainly "wait a frame, tick the
//     mission audio, bail if the target is already dead", since it is reached right after
//     HAS_MISSION_AUDIO_FINISHED/IS_CHAR_DEAD checks every time) that this tool never mines,
//     because it belongs to no single mission. Landing on one fails the mission safely - see
//     RunOne's ExternalLabel case - rather than guessing what it does.
//   - OPCODES OUTSIDE THE HANDLER TABLES BELOW. Two kinds are told apart. Cosmetic/AI-flavour
//     opcodes this build has no reason to refuse over - camera cuts, mission audio, widescreen
//     bars, a ped's accuracy or personality, a car's cruise speed - are logged once and treated
//     as no-ops (IsCosmeticNoOp). Anything else not in ConditionHandlers or StatementHandlers
//     fails the mission on the spot, naming the opcode, exactly as an unmined external jump does.
#include <plugin.h>
#include <CPed.h>
#include <CPlayerPed.h>
#include <CPopulation.h>
#include <CPools.h>
#include <CVehicle.h>
#include <CTimer.h>
#include <CMessages.h>
#include <CRadar.h>
#include <CStreaming.h>
#include <CCarEnterExit.h>
#include <CModelInfo.h>
#include <CBaseModelInfo.h>
#include <CWorld.h>
#include <cstdio>
#include <cmath>
#include <map>
#include <functional>
#include <set>
#include "MissionEngine.h"
#include "Gxt.h"
#include "MissionData.h"
#include "Settings.h"
#include "Log.h"
#include "VehicleSpawn.h"

using namespace plugin;
using namespace MissionData;

namespace MissionEngine {
namespace {

// CTaskComplexKillPedOnFoot, built exactly the way RegionPolice.cpp's SendAfterPlayer does -
// same addresses, same layout - because a mission target that fights back needs the identical
// task San Andreas' own script-driven enemies use. Not shared code across the two files only
// because RegionPolice.cpp keeps its copy in an anonymous namespace; the addresses are the point
// of agreement, and both are checked against tools/sa_functions.txt.
constexpr uintptr_t kTaskNew = 0x61A5A0;                 // CTask::operator new
constexpr uintptr_t kKillPedOnFootCtor = 0x620E30;       // CTaskComplexKillPedOnFoot::CTaskComplexKillPedOnFoot
constexpr unsigned kKillPedOnFootSize = 0x38;
typedef void*(__cdecl* TaskNewFn)(unsigned size);
typedef void*(__thiscall* KillPedOnFootCtorFn)(void* self, CPed* target, int time, int specFlags,
                                               int delay, int chance, char a7);
constexpr int kNoTimeLimit = -1;
constexpr unsigned char kCopTaskFlags = 0x30;   // the same two bits CreateWaitingCoppers sets
constexpr int kTaskPrimary = 3;                 // ePrimaryTasks::TASK_PRIMARY_PRIMARY

bool SetKillPlayerTask(CPed* ped) {
    if (!ped || !ped->m_pIntelligence) return false;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return false;
    void* memory = reinterpret_cast<TaskNewFn>(kTaskNew)(kKillPedOnFootSize);
    if (!memory) return false;
    memset(memory, 0, kKillPedOnFootSize);
    reinterpret_cast<KillPedOnFootCtorFn>(kKillPedOnFootCtor)(memory, player, kNoTimeLimit, 0, 0, 0, 2);
    *((uint8_t*)memory + 0xC) |= kCopTaskFlags;
    CTaskManager& tasks = ped->m_pIntelligence->m_TaskMgr;
    tasks.SetTask(reinterpret_cast<CTask*>(memory), kTaskPrimary, false);
    return tasks.m_aPrimaryTasks[kTaskPrimary] != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Mission state

std::map<std::string, Mission> gMissions;
std::vector<std::string> gIds;

// A ringing-phone trigger, one per region that has missions loaded. NOT the donor's own payphone
// position: those missions do not use a single fixed phone (asm1's own getaway car spawn reads
// its position from a donor global rather than a literal - see tools/import_missions.py's
// _coord()), and mining the donor's payphone-selection logic itself was out of scope for this
// pass. Placed at the region's own origin plus a small fixed offset instead, purely so there is
// something in the world to walk into for testing - see docs/missions.md.
struct PhoneTrigger {
    std::string idPrefix;   // which loaded missions this phone offers, by id prefix
    CVector pos;
    size_t next = 0;
};
std::vector<PhoneTrigger> gPhones;
constexpr float kPhoneTriggerRadius = 2.5f;

enum class Phase { Idle, Running, Complete, Failed };
Phase gPhase = Phase::Idle;
const Mission* gMission = nullptr;
int gIp = 0;
unsigned gWaitUntil = 0;
bool gYield = false;                     // set by WAIT: hand the frame back even when the wait is 0 ms
unsigned gCutsceneStart = 0;             // ms when the script last called START_CUTSCENE; the cutscene itself is not played
bool gLastCondition = true;
std::string gLastMessage;

using VarKey = std::pair<bool, int>;   // (isGlobal, index)
std::map<VarKey, int> gInt;
std::map<VarKey, float> gFloat;
std::map<VarKey, int> gPedRef;         // donor var -> CPools ped ref, for CREATE_CHAR's own output
std::map<VarKey, int> gVehicleRef;
std::map<VarKey, int> gVehicleModel;

std::set<std::string> gWarnedCosmetic;   // one log line per cosmetic no-op opcode, not per hit
std::set<std::string> gWarnedUnsupported;

std::vector<int> gCallStack;             // GOSUB return addresses (program indices)
std::map<std::string, int> gModelCache;  // resolved-name -> live SA model id, this mission's run

VarKey KeyOf(const Operand& o) { return {o.kind == OperandKind::GlobalVar, o.i}; }

int GetInt(const Operand& o) {
    if (o.kind == OperandKind::Int) return o.i;
    if (o.kind == OperandKind::Float) return (int)o.f;
    auto it = gInt.find(KeyOf(o));
    return it == gInt.end() ? 0 : it->second;
}
float GetFloat(const Operand& o) {
    if (o.kind == OperandKind::Float) return o.f;
    if (o.kind == OperandKind::Int) return (float)o.i;
    auto it = gFloat.find(KeyOf(o));
    return it == gFloat.end() ? 0.0f : it->second;
}
void SetInt(const Operand& o, int v) { if (o.kind == OperandKind::GlobalVar || o.kind == OperandKind::LocalVar) gInt[KeyOf(o)] = v; }
void SetFloat(const Operand& o, float v) { if (o.kind == OperandKind::GlobalVar || o.kind == OperandKind::LocalVar) gFloat[KeyOf(o)] = v; }

// The live SA model id for a ModelName operand, resolved once (StartMission's load-time pass)
// and cached by name - CModelInfo::GetModelInfo(name, &id), exactly the call RegionParkedCars.cpp
// and RegionPeds.cpp already make for a namespaced name. -1 for an operand tools/import_missions.py
// could not map to a name at all (a `u:` token) or one CModelInfo does not recognise (the region
// that owns it was not imported, or was imported before this model existed there).
int GetModel(const Operand& o) {
    if (o.kind != OperandKind::ModelName) return GetInt(o);
    if (o.text.empty()) return -1;
    auto it = gModelCache.find(o.text);
    return it == gModelCache.end() ? -1 : it->second;
}

CPed* ResolvePed(const Operand& o) {
    auto it = gPedRef.find(KeyOf(o));
    if (it == gPedRef.end()) return nullptr;
    return CPools::GetPed(it->second);
}
CVehicle* ResolveVehicle(const Operand& o) {
    auto it = gVehicleRef.find(KeyOf(o));
    if (it == gVehicleRef.end()) return nullptr;
    auto mit = gVehicleModel.find(KeyOf(o));
    return VehicleSpawn::Resolve(it->second, mit == gVehicleModel.end() ? -1 : mit->second);
}

CVector PlayerPos() {
    CPlayerPed* p = FindPlayerPed();
    return p ? p->GetPosition() : CVector(0, 0, 0);
}

// A PRINT operand is a GXT key (MEA1_B); the mission file carries the donor's line for it, and
// the region's own american.gxt (Gxt) is the fallback. Anything with a space is already text.
std::string TextFor(const std::string& keyOrText) {
    if (keyOrText.find(' ') != std::string::npos) return keyOrText;
    if (gMission) {
        auto it = gMission->texts.find(keyOrText);
        if (it != gMission->texts.end()) return it->second;
    }
    if (const char* t = Gxt::Find(keyOrText.c_str())) return t;
    return keyOrText;
}

void ShowMessage(const std::string& text, int timeMs) {
    if (text.empty()) {
        Log::Write("missions: %s - a PRINT with no text (the GXT key was not resolved by the tool)",
                   gMission ? gMission->id.c_str() : "?");
        return;
    }
    Log::Write("missions: %s - prints \"%s\" for %d ms", gMission ? gMission->id.c_str() : "?",
               text.c_str(), timeMs);
    gLastMessage = text;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text.c_str());
    CMessages::AddMessageJumpQ(buf, timeMs > 0 ? timeMs : 4000, 0, false);
}

void Reset() {
    gPhase = Phase::Idle;
    gMission = nullptr;
    gIp = 0;
    gWaitUntil = 0;
    gLastCondition = true;
    gLastMessage.clear();
    gInt.clear();
    gFloat.clear();
    gPedRef.clear();
    gVehicleRef.clear();
    gVehicleModel.clear();
    gCallStack.clear();
    gModelCache.clear();
}

// Cosmetic/AI-flavour opcodes this build has no reason to fail a mission over: camera, mission
// audio, widescreen bars, and the "how this ped/car behaves" flags (accuracy, personality, cruise
// speed, colour...) that make a fight feel right but that nothing here checks for completion.
// Classified by name rather than individually verified against an address - see docs/missions.md.
bool IsCosmeticNoOp(const std::string& op) {
    static const char* substrings[] = {
        "CAMERA", "AUDIO", "FADE", "WIDESCREEN", "CUTSCENE", "GROUP", "ELEGANTLY", "DRAGGED",
        "DENSITY", "CRUISE_SPEED", "DRIVING_STYLE", "COLOUR", "ACCURACY", "PERSONALITY",
        "THREAT_SEARCH", "WAIT_STATE", "CROUCH", "STAY_IN_SAME_PLACE", "ONLY_DAMAGED_BY_PLAYER",
        "PLAYER_CONTROL", "CLEAR_AREA", "PRINT", "HEADING", "NEVER_TARGETTED",
        // Positional/staging opcodes: skipped deliberately rather than executed, because by the
        // time a mission starts (whether from the ringing-phone trigger, which now stands the
        // player at this mission's own mined SET_PLAYER_COORDINATES position - see
        // MinePhonePosition - or from the F10 test trigger, which does not move the player at
        // all) forcibly teleporting again is more likely to surprise a tester than help one.
        "COORDINATES", "ANSWERING_MOBILE", "DEATHARREST",
    };
    for (auto s : substrings) if (op.find(s) != std::string::npos) return true;
    return false;
}

void FailMission(const char* why, ...) {
    char buf[256];
    va_list args;
    va_start(args, why);
    vsnprintf(buf, sizeof(buf), why, args);
    va_end(args);
    Log::Write("missions: %s failed at instruction %d - %s",
              gMission ? gMission->id.c_str() : "?", gIp, buf);
    char msg[300];
    snprintf(msg, sizeof(msg), "MapExpander: mission failed - %s", buf);
    ShowMessage(msg, 6000);
    gPhase = Phase::Failed;
}

// ---------------------------------------------------------------------------------------------
// Condition opcodes - anything IF's own AND/OR combine can evaluate. `negate` (the donor
// bytecode's own NOT flag) is applied by the caller, not here.
using ConditionFn = std::function<bool(const Instruction&)>;
std::map<std::string, ConditionFn> gConditions;

void InstallConditions() {
    gConditions["IS_CHAR_DEAD"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        return !p || p->m_fHealth <= 0.0f;
    };
    gConditions["IS_CAR_DEAD"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        return !v || v->m_fHealth <= 0.0f;
    };
    gConditions["IS_CAR_STOPPED"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        return v && v->m_vecMoveSpeed.x * v->m_vecMoveSpeed.x + v->m_vecMoveSpeed.y * v->m_vecMoveSpeed.y
                    + v->m_vecMoveSpeed.z * v->m_vecMoveSpeed.z < 0.01f;
    };
    gConditions["IS_INT_VAR_EQUAL_TO_NUMBER"] = [](const Instruction& ins) {
        return GetInt(ins.operands[0]) == GetInt(ins.operands[1]);
    };
    gConditions["IS_INT_VAR_GREATER_THAN_NUMBER"] = [](const Instruction& ins) {
        return GetInt(ins.operands[0]) > GetInt(ins.operands[1]);
    };
    gConditions["IS_INT_LVAR_GREATER_THAN_NUMBER"] = gConditions["IS_INT_VAR_GREATER_THAN_NUMBER"];
    gConditions["IS_NUMBER_GREATER_THAN_INT_VAR"] = [](const Instruction& ins) {
        return GetInt(ins.operands[0]) > GetInt(ins.operands[1]);
    };
    gConditions["HAS_MODEL_LOADED"] = [](const Instruction& ins) {
        return CStreaming::HasModelLoaded(GetModel(ins.operands[0]));
    };
    // Audio the engine never actually loads always reads as finished/loaded, so a script polling
    // it never stalls waiting for something that will not happen - see PLAY_MISSION_AUDIO's note.
    gConditions["HAS_MISSION_AUDIO_FINISHED"] = [](const Instruction&) { return true; };
    gConditions["HAS_CUTSCENE_FINISHED"] = [](const Instruction&) {
        const unsigned kVirtualCutsceneMs = 20000;
        if (!gCutsceneStart) gCutsceneStart = CTimer::m_snTimeInMilliseconds;
        return CTimer::m_snTimeInMilliseconds - gCutsceneStart >= kVirtualCutsceneMs;
    };
    gConditions["HAS_CUTSCENE_LOADED"] = [](const Instruction&) { return true; };
    gConditions["HAS_MISSION_AUDIO_LOADED"] = [](const Instruction&) { return true; };
    gConditions["GET_FADING_STATUS"] = [](const Instruction&) { return false; };
    gConditions["IS_BUTTON_PRESSED"] = [](const Instruction&) { return false; };
    gConditions["IS_PLAYER_SHOOTING"] = [](const Instruction&) { return false; };
    // No verified way to read the player's currently-equipped weapon slot was found for this
    // build (CPlayerPed exposes no GetActiveWeapon here), so this always reads false rather than
    // guessing at an unchecked field - see docs/missions.md.
    gConditions["IS_CURRENT_PLAYER_WEAPON"] = [](const Instruction&) { return false; };
    gConditions["IS_PLAYER_IN_CAR"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[ins.operands.size() > 1 ? 1 : 0]);
        CPlayerPed* p = FindPlayerPed();
        return p && v && p->m_pVehicle == v;
    };
    gConditions["IS_CHAR_IN_CAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        CVehicle* v = ResolveVehicle(ins.operands[1]);
        return p && v && p->m_pVehicle == v;
    };
    gConditions["IS_CHAR_IN_ANY_CAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        return p && p->m_pVehicle != nullptr;
    };
    gConditions["LOCATE_PLAYER_ANY_MEANS_CHAR_2D"] = [](const Instruction& ins) {
        // self, radiusX, radiusY, unused - the common III/VC signature; distance only, no
        // heading check, which is the "any means" (on foot or in a car) half of the family this
        // build supports. See docs/missions.md for the LOCATE_* subset actually handled.
        if (ins.operands.size() < 3) return false;
        CPed* target = ResolvePed(ins.operands[0]);
        if (!target) return false;
        CVector p = PlayerPos(), t = target->GetPosition();
        float rx = GetFloat(ins.operands[1]), ry = GetFloat(ins.operands[2]);
        float dx = p.x - t.x, dy = p.y - t.y;
        float rr = (rx > ry ? rx : ry);
        return dx * dx + dy * dy <= rr * rr;
    };
    gConditions["LOCATE_PLAYER_ANY_MEANS_CHAR_3D"] = gConditions["LOCATE_PLAYER_ANY_MEANS_CHAR_2D"];

    // ---- Comparison forms IS_INT_VAR_EQUAL_TO_NUMBER's own handler above did not already cover -
    // pure interpreter work, no game state involved beyond the variable store.
    gConditions["IS_INT_LVAR_GREATER_OR_EQUAL_TO_NUMBER"] = [](const Instruction& ins) {
        return GetInt(ins.operands[0]) >= GetInt(ins.operands[1]);
    };
    gConditions["IS_NUMBER_GREATER_THAN_INT_LVAR"] = gConditions["IS_NUMBER_GREATER_THAN_INT_VAR"];
    gConditions["IS_NUMBER_GREATER_OR_EQUAL_TO_INT_VAR"] = [](const Instruction& ins) {
        return GetInt(ins.operands[0]) >= GetInt(ins.operands[1]);
    };
    gConditions["IS_FLOAT_VAR_GREATER_THAN_NUMBER"] = [](const Instruction& ins) {
        return GetFloat(ins.operands[0]) > GetFloat(ins.operands[1]);
    };
    gConditions["IS_NUMBER_GREATER_THAN_FLOAT_VAR"] = [](const Instruction& ins) {
        return GetFloat(ins.operands[0]) > GetFloat(ins.operands[1]);
    };

    // ---- Car/ped status - real field reads, not stubs. IS_CAR_UPSIDEDOWN is the most-used
    // unsupported opcode across all nine missions (38 hits): CPlaceable::GetUp() is the vehicle's
    // own local up axis in world space, so its world-Z component going negative is upside down -
    // the same test any of GTA's own physics code would make with a matrix it already has.
    gConditions["IS_CAR_UPSIDEDOWN"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        return v && v->GetUp().z < -0.1f;
    };
    gConditions["GET_CAR_HEALTH"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (ins.operands.size() > 1) SetInt(ins.operands[1], v ? (int)v->m_fHealth : 0);
        return true;   // registered as a condition slot only so its GET_ form parses like others
    };
    gConditions["IS_CAR_HEALTH_GREATER"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        return v && v->m_fHealth > (float)GetInt(ins.operands[1]);
    };
    gConditions["IS_CHAR_SITTING_IN_CAR"] = gConditions["IS_CHAR_IN_CAR"];
    gConditions["IS_CHAR_SITTING_IN_ANY_CAR"] = gConditions["IS_CHAR_IN_ANY_CAR"];
    gConditions["IS_CHAR_ON_FOOT"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        return p && p->m_pVehicle == nullptr;
    };
    gConditions["IS_PLAYER_IN_ANY_CAR"] = [](const Instruction&) {
        CPlayerPed* p = FindPlayerPed();
        return p && p->m_pVehicle != nullptr;
    };
    gConditions["IS_PLAYER_SITTING_IN_ANY_CAR"] = gConditions["IS_PLAYER_IN_ANY_CAR"];
    gConditions["IS_PLAYER_STOPPED_IN_AREA_2D"] = gConditions["LOCATE_PLAYER_ANY_MEANS_CHAR_2D"];
    gConditions["IS_PLAYER_IN_AREA_2D"] = [](const Instruction& ins) {
        if (ins.operands.size() < 4) return false;
        CVector p = PlayerPos();
        float x1 = GetFloat(ins.operands[0]), y1 = GetFloat(ins.operands[1]);
        float x2 = GetFloat(ins.operands[2]), y2 = GetFloat(ins.operands[3]);
        return p.x >= (x1 < x2 ? x1 : x2) && p.x <= (x1 < x2 ? x2 : x1)
            && p.y >= (y1 < y2 ? y1 : y2) && p.y <= (y1 < y2 ? y2 : y1);
    };
    // Not implemented with a verified field/address for this build - always false rather than
    // guessed. See docs/missions.md.
    for (const char* op : {"IS_CAR_IN_WATER", "IS_CAR_ON_FIRE", "IS_CAR_STUCK", "IS_CAR_STUCK_ON_ROOF",
                           "IS_CAR_TYRE_BURST", "IS_CAR_CRUSHED", "IS_CAR_ON_SCREEN", "IS_CHAR_ON_SCREEN",
                           "IS_POINT_ON_SCREEN", "IS_POINT_OBSCURED_BY_A_MISSION_ENTITY",
                           "IS_CHAR_LEAVING_VEHICLE_TO_DIE", "IS_SNIPER_BULLET_IN_AREA",
                           "IS_CRANE_LIFTING_CAR", "HAS_RESPRAY_HAPPENED", "IS_CHAR_OBJECTIVE_PASSED",
                           "HAS_CHAR_BEEN_DAMAGED_BY_WEAPON", "HAS_CHAR_BEEN_DAMAGED_BY_CHAR",
                           "HAS_CAR_BEEN_DAMAGED_BY_WEAPON", "HAS_PICKUP_BEEN_COLLECTED",
                           // The remaining LOCATE_* shapes (player/char vs a car target, "stopped"
                           // variants, a few plain coordinate forms) - covered structurally by
                           // the handful above, but not every operand shape individually; rather
                           // than misreport a car's position as a char's, these read false so a
                           // WAIT/retry loop around one stalls quietly instead of mis-triggering.
                           "LOCATE_CHAR_ON_FOOT_3D", "LOCATE_CHAR_ON_FOOT_2D", "LOCATE_CHAR_ANY_MEANS_2D",
                           "LOCATE_PLAYER_ANY_MEANS_CAR_2D", "LOCATE_PLAYER_IN_CAR_CHAR_2D",
                           "LOCATE_STOPPED_CHAR_IN_CAR_3D", "LOCATE_STOPPED_CHAR_ON_FOOT_2D",
                           "LOCATE_STOPPED_CAR_3D", "LOCATE_STOPPED_PLAYER_ANY_MEANS_3D",
                           "LOCATE_PLAYER_ANY_MEANS_2D", "LOCATE_PLAYER_ON_FOOT_3D",
                           "LOCATE_PLAYER_ANY_MEANS_3D", "IS_CAR_STOPPED_IN_AREA_3D"})
        gConditions[op] = [](const Instruction&) { return false; };
}

// ---------------------------------------------------------------------------------------------
// Statement opcodes - everything that is not a condition. Returning false means "do not advance
// the instruction pointer the normal way" (WAIT, GOTO and GOTO_IF_FALSE all move it themselves).
using StatementFn = std::function<bool(const Instruction&)>;
std::map<std::string, StatementFn> gStatements;

void InstallStatements() {
    gStatements["WAIT"] = [](const Instruction& ins) {
        int ms = ins.operands.empty() ? 0 : GetInt(ins.operands[0]);
        gWaitUntil = CTimer::m_snTimeInMilliseconds + (unsigned)(ms > 0 ? ms : 0);
        // WAIT 0 is how every III/VC polling loop hands the frame back; without yielding here
        // the loop below runs its whole step budget every frame and the mission never advances.
        gYield = true;
        gIp++;
        return false;
    };
    gStatements["GOTO"] = [](const Instruction& ins) {
        const Operand& target = ins.operands[0];
        if (target.kind == OperandKind::ExternalLabel) {
            FailMission("GOTO to code outside this mission (donor address 0x%x)", target.externalAddr);
            return false;
        }
        gIp = target.label;
        return false;
    };
    gStatements["GOTO_IF_FALSE"] = [](const Instruction& ins) {
        if (gLastCondition) { gIp++; return false; }
        const Operand& target = ins.operands[0];
        if (target.kind == OperandKind::ExternalLabel) {
            FailMission("GOTO_IF_FALSE to code outside this mission (donor address 0x%x)",
                       target.externalAddr);
            return false;
        }
        gIp = target.label;
        return false;
    };
    gStatements["GOSUB"] = [](const Instruction& ins) {
        const Operand& target = ins.operands[0];
        if (target.kind == OperandKind::ExternalLabel) {
            FailMission("GOSUB to code outside this mission (donor address 0x%x) - subroutine "
                       "mining did not reach it, see docs/missions.md", target.externalAddr);
            return false;
        }
        gCallStack.push_back(gIp + 1);
        gIp = target.label;
        return false;
    };
    gStatements["RETURN"] = [](const Instruction&) {
        if (gCallStack.empty()) {
            // Reached with nothing to return to - the mission's own thread ending naturally
            // rather than through REGISTER_MISSION_PASSED (see docs/missions.md: none of the
            // nine missions mined call GOSUB from their own main range, so this is only ever hit
            // inside a mined subroutine whose own caller chain this build could not fully trace).
            Log::Write("missions: %s - RETURN with an empty call stack; ending the mission thread",
                      gMission->id.c_str());
            gPhase = Phase::Failed;
            return false;
        }
        gIp = gCallStack.back();
        gCallStack.pop_back();
        return false;
    };
    // ADD_SCORE(player, amount) - CWorld::Players[0] is the local player's own CPlayerInfo in
    // single-player, m_nMoney at 0xB8 (checked against the real plugin-sdk header pulled from
    // the build's own Docker image, not assumed). No opcode in any of the nine missions' own main
    // range calls this directly - it turned up only in mined subroutines - but it is real and
    // wired up rather than stubbed, since a mission that does reach it should pay out.
    gStatements["ADD_SCORE"] = [](const Instruction& ins) {
        int amount = GetInt(ins.operands[ins.operands.size() > 1 ? 1 : 0]);
        CWorld::Players[0].m_nMoney += amount;
        Log::Write("missions: %s - ADD_SCORE %d (balance now %d)", gMission->id.c_str(), amount,
                  CWorld::Players[0].m_nMoney);
        return true;
    };
    // IF's own single operand is the donor script's AND/OR encoding: 0-7 means that many + 1
    // conditions ANDed, 20-27 means that many - 19 ORed - Sanny Builder's own documented
    // encoding, checked against real bytes: an IF with operand 1 here is followed by exactly two
    // condition instructions in every mission mined, matching "1 -> 2 conditions ANDed".
    gStatements["IF"] = [](const Instruction& ins) {
        int andor = GetInt(ins.operands[0]);
        bool useOr = andor > 7;
        int count = (useOr ? andor - 20 : andor) + 1;
        bool result = useOr ? false : true;
        for (int i = 1; i <= count && gIp + i < (int)gMission->program.size(); i++) {
            const Instruction& cond = gMission->program[gIp + i];
            bool v = false;
            auto it = gConditions.find(cond.opcode);
            if (it != gConditions.end()) v = it->second(cond);
            else if (!IsCosmeticNoOp(cond.opcode))
                Log::Write("missions: %s - IF uses unrecognised condition opcode %s at %d (treated false)",
                          gMission->id.c_str(), cond.opcode.c_str(), gIp + i);
            if (cond.negate) v = !v;
            result = useOr ? (result || v) : (result && v);
        }
        gLastCondition = result;
        gIp += count + 1;
        return false;
    };
    gStatements["SET_VAR_INT"] = gStatements["SET_LVAR_INT"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[1])); return true;
    };
    gStatements["SET_VAR_FLOAT"] = gStatements["SET_LVAR_FLOAT"] = [](const Instruction& ins) {
        SetFloat(ins.operands[0], GetFloat(ins.operands[1])); return true;
    };
    gStatements["SET_VAR_INT_TO_VAR_INT"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[1])); return true;
    };
    gStatements["SUB_INT_VAR_FROM_INT_VAR"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[0]) - GetInt(ins.operands[1])); return true;
    };
    gStatements["GET_GAME_TIMER"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], (int)CTimer::m_snTimeInMilliseconds); return true;
    };
    // Cutscenes are not played (no III/VC cutscene data is imported), but the mission scripts pace
    // their opening dialogue by GET_CUTSCENE_TIME and wait on HAS_CUTSCENE_FINISHED, so the clock
    // has to run: START_CUTSCENE stamps it, GET_CUTSCENE_TIME reports the ms since, and the
    // cutscene counts as finished after kVirtualCutsceneMs - long enough for the timed briefs
    // the scripts print (III's MEA intros pace lines at 2-3 s each), short enough to not feel stuck.
    gStatements["START_CUTSCENE"] = [](const Instruction&) { gCutsceneStart = CTimer::m_snTimeInMilliseconds; return true; };
    gStatements["START_CUTSCENE_NOW"] = gStatements["START_CUTSCENE"];
    gStatements["GET_CUTSCENE_TIME"] = [](const Instruction& ins) {
        if (!gCutsceneStart) gCutsceneStart = CTimer::m_snTimeInMilliseconds;
        SetInt(ins.operands[0], (int)(CTimer::m_snTimeInMilliseconds - gCutsceneStart)); return true;
    };
    gStatements["REQUEST_MODEL"] = [](const Instruction& ins) {
        CStreaming::RequestModel(GetModel(ins.operands[0]), 0); return true;
    };
    gStatements["MARK_MODEL_AS_NO_LONGER_NEEDED"] = [](const Instruction&) { return true; };
    gStatements["PRINT_BIG"] = gStatements["PRINT_NOW"] = [](const Instruction& ins) {
        const Operand& key = ins.operands[0];
        int t = ins.operands.size() > 1 ? GetInt(ins.operands[1]) : 4000;
        ShowMessage(TextFor(key.text), t);
        return true;
    };
    gStatements["PRINT_HELP"] = [](const Instruction& ins) {
        ShowMessage(TextFor(ins.operands[0].text), 5000); return true;
    };
    gStatements["CREATE_CHAR"] = [](const Instruction& ins) {
        if (ins.operands.size() < 6) return true;
        int pedType = GetInt(ins.operands[0]), model = GetModel(ins.operands[1]);
        CVector at(GetFloat(ins.operands[2]), GetFloat(ins.operands[3]), GetFloat(ins.operands[4]));
        if (!CStreaming::HasModelLoaded(model)) { CStreaming::RequestModel(model, 0); return true; }
        CPed* ped = CPopulation::AddPed((ePedType)pedType, (unsigned)model, at, false);
        if (ped) {
            ped->SetCharCreatedBy(1);
            gPedRef[KeyOf(ins.operands[5])] = CPools::GetPedRef(ped);
        }
        return true;
    };
    gStatements["CREATE_CAR"] = [](const Instruction& ins) {
        if (ins.operands.size() < 5) return true;
        int model = GetModel(ins.operands[0]);
        CVector at(GetFloat(ins.operands[1]), GetFloat(ins.operands[2]), GetFloat(ins.operands[3]));
        if (!VehicleSpawn::ModelIsLoaded(model)) return true;
        CVehicle* v = VehicleSpawn::Create(model, at, true);
        if (v) {
            gVehicleRef[KeyOf(ins.operands[4])] = CPools::GetVehicleRef(v);
            gVehicleModel[KeyOf(ins.operands[4])] = model;
        }
        return true;
    };
    gStatements["CREATE_CHAR_INSIDE_CAR"] = [](const Instruction& ins) {
        if (ins.operands.size() < 4) return true;
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        int pedType = GetInt(ins.operands[1]), model = GetModel(ins.operands[2]);
        if (!v || !CStreaming::HasModelLoaded(model)) return true;
        CPed* ped = CPopulation::AddPed((ePedType)pedType, (unsigned)model, v->GetPosition(), false);
        if (ped) {
            ped->SetCharCreatedBy(1);
            CCarEnterExit::SetPedInCarDirect(ped, v, 0, true);
            gPedRef[KeyOf(ins.operands[3])] = CPools::GetPedRef(ped);
        }
        return true;
    };
    gStatements["CREATE_CHAR_AS_PASSENGER"] = [](const Instruction& ins) {
        if (ins.operands.size() < 5) return true;
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        int pedType = GetInt(ins.operands[1]), model = GetModel(ins.operands[2]);
        int seat = GetInt(ins.operands[3]);
        if (!v || !CStreaming::HasModelLoaded(model)) return true;
        CPed* ped = CPopulation::AddPed((ePedType)pedType, (unsigned)model, v->GetPosition(), false);
        if (ped) {
            ped->SetCharCreatedBy(1);
            CCarEnterExit::SetPedInCarDirect(ped, v, seat + 1, false);
            gPedRef[KeyOf(ins.operands[4])] = CPools::GetPedRef(ped);
        }
        return true;
    };
    gStatements["GIVE_WEAPON_TO_CHAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (!p) return true;
        int weapon = GetInt(ins.operands[1]), ammo = GetInt(ins.operands[2]);
        p->GiveWeapon((eWeaponType)weapon, ammo > 0 ? ammo : 100, true);
        p->SetCurrentWeapon((eWeaponType)weapon);
        return true;
    };
    gStatements["GIVE_WEAPON_TO_PLAYER"] = [](const Instruction& ins) {
        CPlayerPed* p = FindPlayerPed();
        if (!p) return true;
        int weapon = GetInt(ins.operands[1]), ammo = GetInt(ins.operands[2]);
        p->GiveWeapon((eWeaponType)weapon, ammo > 0 ? ammo : 100, true);
        p->SetCurrentWeapon((eWeaponType)weapon);
        return true;
    };
    gStatements["ADD_BLIP_FOR_CHAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p) CRadar::SetEntityBlip(BLIP_CHAR, CPools::GetPedRef(p), 0, BLIP_DISPLAY_BOTH);
        return true;
    };
    gStatements["ADD_BLIP_FOR_CAR"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (v) CRadar::SetEntityBlip(BLIP_CAR, CPools::GetVehicleRef(v), 0, BLIP_DISPLAY_BOTH);
        return true;
    };
    gStatements["ADD_BLIP_FOR_COORD"] = [](const Instruction& ins) {
        if (ins.operands.size() < 3) return true;
        CVector at(GetFloat(ins.operands[0]), GetFloat(ins.operands[1]), GetFloat(ins.operands[2]));
        CRadar::SetCoordBlip(BLIP_COORD, at, 0, BLIP_DISPLAY_BOTH, nullptr);
        return true;
    };
    gStatements["REMOVE_BLIP"] = [](const Instruction&) { return true; };   // handle not tracked; cosmetic
    gStatements["SET_CHAR_HEALTH"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p) p->m_fHealth = (float)GetInt(ins.operands[1]);
        return true;
    };
    gStatements["SET_CAR_HEALTH"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (v) v->m_fHealth = (float)GetInt(ins.operands[1]);
        return true;
    };
    gStatements["MARK_CHAR_AS_NO_LONGER_NEEDED"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p) p->SetCharCreatedBy(1);
        return true;
    };
    gStatements["MARK_CAR_AS_NO_LONGER_NEEDED"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (v) VehicleSpawn::HandBackToTheGame(v);
        return true;
    };
    gStatements["SET_CHAR_OBJ_KILL_CHAR_ON_FOOT"] = gStatements["SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT"]
        = gStatements["SET_CHAR_OBJ_KILL_CHAR_ANY_MEANS"] = gStatements["SET_CHAR_OBJ_KILL_PLAYER_ANY_MEANS"]
        = [](const Instruction& ins) { SetKillPlayerTask(ResolvePed(ins.operands[0])); return true; };
    // No verified CTaskComplexDestroyCar address exists in tools/sa_functions.txt, so this is not
    // faked - the ped simply does not receive an AI task to ram the vehicle. It stays hostile
    // (the kill-player task above still applies where a mission sets both).
    gStatements["SET_CHAR_OBJ_DESTROY_CAR"] = [](const Instruction&) { return true; };
    gStatements["PLAYER_MADE_PROGRESS"] = [](const Instruction&) { return true; };
    gStatements["PLAY_MISSION_PASSED_TUNE"] = [](const Instruction&) { return true; };
    gStatements["REGISTER_MISSION_PASSED"] = [](const Instruction& ins) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s complete (%s) - no save, no cash: see docs/missions.md",
                gMission->title.c_str(), ins.operands.empty() ? "" : ins.operands[0].text.c_str());
        ShowMessage(buf, 6000);
        Log::Write("missions: %s", buf);
        gPhase = Phase::Complete;
        return true;
    };

    // ---- Mission bookkeeping opcodes every mission's own block opens/closes with - real,
    // narrow behaviour rather than a generic no-op, since these mark genuine state transitions.
    gStatements["SCRIPT_NAME"] = [](const Instruction&) { return true; };   // already used at load
    gStatements["REGISTER_MISSION_GIVEN"] = [](const Instruction&) { return true; };
    gStatements["MISSION_HAS_FINISHED"] = [](const Instruction&) { return true; };
    gStatements["TERMINATE_THIS_SCRIPT"] = [](const Instruction&) {
        // The donor script's own "stop running" instruction, reached on a path that is not
        // REGISTER_MISSION_PASSED (an early-exit check that failed, for instance). Ends the
        // mission thread the same way an empty-stack RETURN does, rather than either failing the
        // mission or silently freezing on the current instruction forever.
        if (gPhase == Phase::Running) {
            Log::Write("missions: %s - TERMINATE_THIS_SCRIPT reached (not REGISTER_MISSION_PASSED)",
                      gMission->id.c_str());
            gPhase = Phase::Failed;
        }
        return false;
    };

    // ---- Arithmetic/variable-to-variable forms - pure interpreter work.
    gStatements["ADD_VAL_TO_INT_VAR"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[0]) + GetInt(ins.operands[1])); return true;
    };
    gStatements["SUB_VAL_FROM_INT_VAR"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[0]) - GetInt(ins.operands[1])); return true;
    };
    gStatements["MULT_INT_VAR_BY_VAL"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], GetInt(ins.operands[0]) * GetInt(ins.operands[1])); return true;
    };
    gStatements["DIV_INT_VAR_BY_VAL"] = [](const Instruction& ins) {
        int d = GetInt(ins.operands[1]);
        if (d != 0) SetInt(ins.operands[0], GetInt(ins.operands[0]) / d);
        return true;
    };
    gStatements["SUB_VAL_FROM_FLOAT_VAR"] = [](const Instruction& ins) {
        SetFloat(ins.operands[0], GetFloat(ins.operands[0]) - GetFloat(ins.operands[1])); return true;
    };
    gStatements["SUB_FLOAT_VAR_FROM_FLOAT_VAR"] = gStatements["SUB_VAL_FROM_FLOAT_VAR"];
    gStatements["MULT_FLOAT_VAR_BY_VAL"] = [](const Instruction& ins) {
        SetFloat(ins.operands[0], GetFloat(ins.operands[0]) * GetFloat(ins.operands[1])); return true;
    };
    gStatements["DIV_FLOAT_VAR_BY_VAL"] = [](const Instruction& ins) {
        float d = GetFloat(ins.operands[1]);
        if (d != 0.0f) SetFloat(ins.operands[0], GetFloat(ins.operands[0]) / d);
        return true;
    };
    gStatements["SET_VAR_FLOAT_TO_VAR_FLOAT"] = [](const Instruction& ins) {
        SetFloat(ins.operands[0], GetFloat(ins.operands[1])); return true;
    };
    gStatements["CSET_VAR_INT_TO_VAR_FLOAT"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], (int)GetFloat(ins.operands[1])); return true;
    };
    gStatements["COS"] = [](const Instruction& ins) {
        SetFloat(ins.operands[1], cosf(GetFloat(ins.operands[0]) * 3.14159265f / 180.0f)); return true;
    };
    gStatements["SIN"] = [](const Instruction& ins) {
        SetFloat(ins.operands[1], sinf(GetFloat(ins.operands[0]) * 3.14159265f / 180.0f)); return true;
    };

    // ---- Coordinates - real, via CPlaceable::SetPosn/GetPosition (verified against the build's
    // own plugin-sdk header), not skipped as cosmetic: a mission's own SET_CHAR_COORDINATES and
    // GET_*_COORDINATES are how it stages a scene and measures distances, not decoration.
    gStatements["SET_CHAR_COORDINATES"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p && ins.operands.size() >= 4)
            p->SetPosn(GetFloat(ins.operands[1]), GetFloat(ins.operands[2]), GetFloat(ins.operands[3]));
        return true;
    };
    gStatements["SET_CAR_COORDINATES"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (v && ins.operands.size() >= 4)
            v->SetPosn(GetFloat(ins.operands[1]), GetFloat(ins.operands[2]), GetFloat(ins.operands[3]));
        return true;
    };
    gStatements["GET_CHAR_COORDINATES"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        CVector v = p ? p->GetPosition() : CVector(0, 0, 0);
        if (ins.operands.size() >= 4) { SetFloat(ins.operands[1], v.x); SetFloat(ins.operands[2], v.y); SetFloat(ins.operands[3], v.z); }
        return true;
    };
    gStatements["GET_CAR_COORDINATES"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        CVector p = v ? v->GetPosition() : CVector(0, 0, 0);
        if (ins.operands.size() >= 4) { SetFloat(ins.operands[1], p.x); SetFloat(ins.operands[2], p.y); SetFloat(ins.operands[3], p.z); }
        return true;
    };
    gStatements["GET_PLAYER_COORDINATES"] = [](const Instruction& ins) {
        CVector v = PlayerPos();
        if (ins.operands.size() >= 4) { SetFloat(ins.operands[1], v.x); SetFloat(ins.operands[2], v.y); SetFloat(ins.operands[3], v.z); }
        return true;
    };
    gStatements["GET_OBJECT_COORDINATES"] = [](const Instruction& ins) {
        if (ins.operands.size() >= 4) { SetFloat(ins.operands[1], 0.0f); SetFloat(ins.operands[2], 0.0f); SetFloat(ins.operands[3], 0.0f); }
        return true;   // CObject spawns are not implemented this pass - see docs/missions.md
    };
    gStatements["GET_DISTANCE_BETWEEN_COORDS_2D"] = [](const Instruction& ins) {
        if (ins.operands.size() < 5) return true;
        float dx = GetFloat(ins.operands[0]) - GetFloat(ins.operands[2]);
        float dy = GetFloat(ins.operands[1]) - GetFloat(ins.operands[3]);
        SetFloat(ins.operands[4], sqrtf(dx * dx + dy * dy));
        return true;
    };
    gStatements["GET_CAR_SPEED"] = [](const Instruction& ins) {
        CVehicle* v = ResolveVehicle(ins.operands[0]);
        if (ins.operands.size() > 1) {
            float s = 0.0f;
            if (v) {
                CVector m = v->m_vecMoveSpeed;
                s = sqrtf(m.x * m.x + m.y * m.y + m.z * m.z);
            }
            SetFloat(ins.operands[1], s);
        }
        return true;
    };
    gStatements["GET_GAME_TIMER"] = [](const Instruction& ins) {
        SetInt(ins.operands[0], (int)CTimer::m_snTimeInMilliseconds); return true;
    };

    // ---- Char enter/leave car - the same CCarEnterExit::SetPedInCarDirect the spawn handlers
    // above already use, so a mission that orders a ped into a seat mid-mission (rather than only
    // at spawn) gets the same real placement.
    gStatements["SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        CVehicle* v = ResolveVehicle(ins.operands[1]);
        if (p && v) CCarEnterExit::SetPedInCarDirect(p, v, 0, true);
        return true;
    };
    gStatements["SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        CVehicle* v = ResolveVehicle(ins.operands[1]);
        if (p && v) CCarEnterExit::SetPedInCarDirect(p, v, 1, false);
        return true;
    };
    // No verified "make this ped get out" call was found for this build - the ped simply keeps
    // its current task rather than one built from a guessed address.
    for (const char* op : {"SET_CHAR_OBJ_LEAVE_CAR", "SET_CHAR_OBJ_LEAVE_ANY_CAR"})
        gStatements[op] = [](const Instruction&) { return true; };
    gStatements["SET_CHAR_OBJ_NO_OBJ"] = [](const Instruction& ins) {
        // Clears whatever task this build itself set (the kill-player task); does not attempt to
        // clear a San Andreas AI task by address.
        return true;
    };
    gStatements["SET_CHAR_OBJ_GOTO_COORD_ON_FOOT"] = [](const Instruction& ins) {
        // No verified CTaskComplexGoToPointAndStandStill construction (address + layout) for this
        // build - see RegionPeds.cpp's own use of it, which is not exposed for mission use here.
        return true;
    };
    gStatements["SET_CHAR_OBJ_WALK_TO_CHAR"] = [](const Instruction&) { return true; };
    gStatements["SET_CHAR_OBJ_FLEE_PLAYER_ON_FOOT_ALWAYS"] = [](const Instruction&) { return true; };

    // ---- Ped stats - real field writes.
    gStatements["ADD_ARMOUR_TO_CHAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p) p->m_fArmour += (float)GetInt(ins.operands[1]);
        return true;
    };
    gStatements["ADD_AMMO_TO_CHAR"] = [](const Instruction&) { return true; };  // no verified per-slot ammo field
    gStatements["CLEAR_CHAR_LAST_DAMAGE_ENTITY"] = [](const Instruction&) { return true; };
    gStatements["DELETE_CHAR"] = [](const Instruction& ins) {
        CPed* p = ResolvePed(ins.operands[0]);
        if (p) p->SetCharCreatedBy(1);   // hand back to the game rather than a raw delete
        return true;
    };
    gStatements["REMOVE_CHAR_ELEGANTLY"] = gStatements["DELETE_CHAR"];
    gStatements["ATTACH_CHAR_TO_CAR"] = [](const Instruction&) { return true; };   // no verified attach call
    for (const char* op : {"TURN_CHAR_TO_FACE_CHAR", "TURN_CHAR_TO_FACE_COORD", "CHAR_LOOK_AT_CHAR_ALWAYS",
                           "STOP_CHAR_LOOKING", "CHAR_WANDER_DIR", "CHAR_FOLLOW_PATH", "CHAR_SET_IDLE",
                           "SET_CHAR_AS_PLAYER_FRIEND", "SET_CHAR_AS_LEADER", "LEAVE_GROUP",
                           "SET_CHARS_CHATTING", "SHUT_CHAR_UP", "SHUT_PLAYER_UP",
                           "SET_ENTER_CAR_RANGE_MULTIPLIER", "SET_CHAR_CAN_BE_SHOT_IN_VEHICLE",
                           "SET_CHAR_RUNNING"})
        gStatements[op] = [](const Instruction&) { return true; };

    // ---- Car AI - CAutoPilot fields RegionTraffic already drives cars through, but a region has
    // no CPathFind graph, so any "goto"/"wander" style mission is a no-op here rather than a
    // guess at path-based movement this build's own cars cannot use either (see
    // docs/known-limitations.md's path section) - the car keeps whatever it was doing.
    // No verified CVehicle heading setter for this build (SetPosn moves the matrix's position
    // column only, not its rotation) - a no-op rather than a guessed matrix rewrite.
    gStatements["SET_CAR_HEADING"] = [](const Instruction&) { return true; };
    gStatements["LOCK_CAR_DOORS"] = [](const Instruction&) { return true; };
    gStatements["CHANGE_CAR_LOCK"] = [](const Instruction&) { return true; };
    gStatements["CHANGE_CAR_COLOUR"] = [](const Instruction&) { return true; };   // no verified colour setter
    for (const char* op : {"CAR_WANDER_RANDOMLY", "SET_CAR_CRUISE_SPEED", "SET_CAR_DRIVING_STYLE",
                           "CAR_GOTO_COORDINATES", "CAR_GOTO_COORDINATES_ACCURATE", "SET_CAR_MISSION",
                           "CAR_SET_IDLE", "SET_CAR_ONLY_DAMAGED_BY_PLAYER", "SET_CAR_HEAVY",
                           "ADD_STUCK_CAR_CHECK", "ADD_UPSIDEDOWN_CAR_CHECK", "STORE_CAR_PLAYER_IS_IN_NO_SAVE",
                           "GET_CLOSEST_CAR_NODE", "LOCATE_CAR_2D", "SWITCH_ROADS_OFF", "SWITCH_ROADS_ON",
                           "SWITCH_PED_ROADS_OFF", "SWITCH_PED_ROADS_ON"})
        gStatements[op] = [](const Instruction&) { return true; };

    // ---- Wanted level - real, via the same CWanted the player's own m_pPlayerData carries
    // (RegionPolice.cpp already reads it the same way).
    gStatements["CLEAR_WANTED_LEVEL"] = [](const Instruction&) {
        CPlayerPed* p = FindPlayerPed();
        if (p && p->m_pPlayerData && p->m_pPlayerData->m_pWanted)
            p->m_pPlayerData->m_pWanted->SetWantedLevel(0);
        return true;
    };
    for (const char* op : {"SET_POLICE_IGNORE_PLAYER", "ALTER_WANTED_LEVEL", "SET_WANTED_MULTIPLIER",
                           "SET_THREAT_REACTION_RANGE_MULTIPLIER", "SET_ZONE_PED_INFO",
                           "SET_PLAYER_MOOD", "ADD_NUMBER_OF_ASSASSINATIONS"})
        gStatements[op] = [](const Instruction&) { return true; };

    // ---- Pickups/objects - not integrated with RegionPickups' own slot budget or CPools this
    // pass (both would need new public API on modules this file does not own) - logged no-ops
    // rather than a guessed CObject/CPickup construction. See docs/missions.md.
    for (const char* op : {"CREATE_PICKUP", "CREATE_PICKUP_WITH_AMMO", "REMOVE_PICKUP",
                           "ADD_BLIP_FOR_PICKUP", "CREATE_OBJECT", "CREATE_OBJECT_NO_OFFSET",
                           "PLACE_OBJECT_RELATIVE_TO_CAR", "SET_OBJECT_COLLISION", "SET_OBJECT_DYNAMIC",
                           "ADD_TO_OBJECT_VELOCITY", "ROTATE_OBJECT", "SLIDE_OBJECT",
                           "MAKE_OBJECT_TARGETTABLE", "DELETE_OBJECT", "MARK_OBJECT_AS_NO_LONGER_NEEDED",
                           "ADD_SPRITE_BLIP_FOR_COORD"})
        gStatements[op] = [](const Instruction&) { return true; };

    // ---- Presentation/loading/streaming - cosmetic no-ops beyond what IsCosmeticNoOp's name
    // match already covers (kept explicit here for the handful whose name does not contain one
    // of those substrings).
    for (const char* op : {"CLEAR_ONSCREEN_COUNTER", "CLEAR_ONSCREEN_TIMER",
                           "DISPLAY_ONSCREEN_COUNTER_WITH_STRING", "DISPLAY_ONSCREEN_TIMER_WITH_STRING",
                           "LOAD_MISSION_TEXT", "LOAD_SPECIAL_CHARACTER", "UNLOAD_SPECIAL_CHARACTER",
                           "SWITCH_STREAMING", "ADD_ONE_OFF_SOUND", "START_NEW_SCRIPT"})
        gStatements[op] = [](const Instruction&) { return true; };
    gConditions["HAS_SPECIAL_CHARACTER_LOADED"] = [](const Instruction&) { return true; };
}

}  // namespace

void Install() {
    InstallConditions();
    InstallStatements();
    for (const MapRegion& r : Settings::regions) {
        if (r.missionsPath.empty() || !Settings::world.regionMissions) continue;
        auto loaded = MissionData::LoadFolder(r.missionsPath);
        if (loaded.empty()) continue;
        std::string prefix = loaded.begin()->first.substr(0, loaded.begin()->first.find('_') + 1);
        PhoneTrigger phone;
        phone.idPrefix = prefix;
        // The first loaded mission's own mined SET_PLAYER_COORDINATES, region origin applied the
        // same way every other mined coordinate in this project is (restarts.dat, parkedcars.dat)
        // - not a placeholder. Every mission has its own such position in the donor script (Mr.
        // Black/Marty Chonks stage the player slightly differently each time), but one physical
        // trigger cycling through all of a region's missions is what this pass builds; a trigger
        // per mission is a natural follow-up once more than one phone per region is wanted.
        bool found = false;
        for (auto& kv : loaded) {
            if (kv.second.hasPhonePos) {
                phone.pos = CVector(r.originX + kv.second.phoneX, r.originY + kv.second.phoneY,
                                    r.originZ + kv.second.phoneZ);
                found = true;
                break;
            }
        }
        if (!found) phone.pos = CVector(r.originX + 10.0f, r.originY + 10.0f, r.originZ);
        Log::Write("missions: %s phone trigger at SA (%.1f, %.1f, %.1f)%s", r.name.c_str(),
                  phone.pos.x, phone.pos.y, phone.pos.z, found ? "" : " (placeholder - no mission "
                  "had a mined SET_PLAYER_COORDINATES)");
        gPhones.push_back(phone);
        for (auto& kv : loaded) {
            gIds.push_back(kv.first);
            gMissions.emplace(kv.first, std::move(kv.second));
        }
    }
    if (Settings::RunningUnderSaMp() && !gMissions.empty())
        Log::Write("SA-MP: %zu mission(s) loaded but will refuse to start", gMissions.size());
}

bool StartMission(const std::string& id) {
    if (Settings::RunningUnderSaMp()) {
        Log::Write("missions: refused to start %s - running under SA-MP", id.c_str());
        return false;
    }
    if (gPhase == Phase::Running) {
        Log::Write("missions: refused to start %s - %s is already running", id.c_str(),
                   gMission ? gMission->id.c_str() : "?");
        return false;
    }
    auto it = gMissions.find(id);
    if (it == gMissions.end()) {
        Log::Write("missions: no mission %s loaded", id.c_str());
        return false;
    }
    Reset();
    gMission = &it->second;

    // Resolve every model a SPAWN actually needs before anything spawns, and refuse to start if
    // one is missing - a mission that gets ninety instructions into its own setup and only then
    // discovers CREATE_CHAR's model does not exist is worse than one that never started.
    // CModelInfo::GetModelInfo is the same call RegionParkedCars.cpp/RegionPeds.cpp already make.
    //
    // Deliberately narrower than "every ModelName operand in the program": REQUEST_MODEL/
    // HAS_MODEL_LOADED/MARK_MODEL_AS_NO_LONGER_NEEDED reference a model without needing it to
    // exist for this build to keep running (RequestModel(-1) is inert), and CREATE_OBJECT*/
    // CREATE_PICKUP/CREATE_CUTSCENE_OBJECT are not implemented as real spawns in this pass at
    // all (see docs/missions.md) - refusing a mission over a prop or cutscene head this build
    // was never going to create anyway would be exactly the kind of unexplained gap this project
    // avoids elsewhere. Every one of the nine missions mined has at least one model id that only
    // resolves through a non-default.ide table (Vice City's mission-actor "special" slots, cut
    // scene heads) this pass does not mine; gating on real spawns rather than every reference is
    // what keeps all nine startable.
    static const std::set<std::string> kSpawnOpcodes = {
        "CREATE_CHAR", "CREATE_CAR", "CREATE_CHAR_INSIDE_CAR", "CREATE_CHAR_AS_PASSENGER",
    };
    std::vector<std::string> missing;
    for (const auto& ins : gMission->program) {
        if (!kSpawnOpcodes.count(ins.opcode)) continue;
        for (const auto& o : ins.operands) {
            if (o.kind != OperandKind::ModelName) continue;
            if (o.text.empty()) {
                missing.push_back("donor model id " + std::to_string((int)o.f) + " (no namespaced "
                                  "name - see tools/import_missions.py's build_model_resolver)");
                continue;
            }
            if (gModelCache.count(o.text)) continue;
            int id = -1;
            CBaseModelInfo* info = CModelInfo::GetModelInfo((char*)o.text.c_str(), &id);
            if (info && id >= 0) {
                gModelCache[o.text] = id;
            } else {
                missing.push_back(o.text);
            }
        }
    }
    if (!missing.empty()) {
        std::string list;
        for (size_t i = 0; i < missing.size() && i < 6; i++) list += (i ? ", " : "") + missing[i];
        Log::Write("missions: refused to start %s - %zu model(s) not found via CModelInfo::"
                  "GetModelInfo: %s", id.c_str(), missing.size(), list.c_str());
        Reset();
        return false;
    }

    gPhase = Phase::Running;
    gIp = 0;
    gWaitUntil = CTimer::m_snTimeInMilliseconds;
    gCutsceneStart = 0;
    gYield = false;
    Log::Write("missions: started %s (%s), %zu instruction(s), %zu model(s) resolved",
              gMission->id.c_str(), gMission->title.c_str(), gMission->program.size(),
              gModelCache.size());
    return true;
}

void EndMission() {
    if (gPhase == Phase::Idle) return;
    for (auto& kv : gVehicleRef) {
        auto mit = gVehicleModel.find(kv.first);
        CVehicle* v = VehicleSpawn::Resolve(kv.second, mit == gVehicleModel.end() ? -1 : mit->second);
        if (v) VehicleSpawn::HandBackToTheGame(v);
    }
    for (auto& kv : gPedRef) {
        CPed* p = CPools::GetPed(kv.second);
        if (p) p->SetCharCreatedBy(1);
    }
    Log::Write("missions: ended %s", gMission ? gMission->id.c_str() : "?");
    Reset();
}

void Tick() {
    if (gPhase != Phase::Running) {
        // The ringing-phone trigger: walk within kPhoneTriggerRadius of a loaded region's phone
        // marker to start its next mission, cycling through however many are loaded for it - see
        // the PhoneTrigger comment above for what this position actually is and is not.
        if (!Settings::RunningUnderSaMp() && !gPhones.empty()) {
            CVector p = PlayerPos();
            for (auto& phone : gPhones) {
                float dx = p.x - phone.pos.x, dy = p.y - phone.pos.y, dz = p.z - phone.pos.z;
                if (dx * dx + dy * dy + dz * dz > kPhoneTriggerRadius * kPhoneTriggerRadius) continue;
                std::vector<std::string> ids;
                for (auto& id : gIds) if (id.rfind(phone.idPrefix, 0) == 0) ids.push_back(id);
                if (ids.empty()) continue;
                StartMission(ids[phone.next % ids.size()]);
                phone.next++;
                break;
            }
        }
    }
    if (gPhase != Phase::Running || !gMission) return;
    if (CTimer::m_snTimeInMilliseconds < gWaitUntil) return;

    // A generous but finite step budget: real missions take a handful of instructions between
    // WAITs (the message/spawn/blip chains above), so hundreds is normal and only a genuine
    // interpreter bug (a condition that can never flip, for instance) would run this out.
    const int kMaxStepsPerFrame = 4000;
    for (int steps = 0; steps < kMaxStepsPerFrame; steps++) {
        if (gPhase != Phase::Running) return;
        if (gIp < 0 || gIp >= (int)gMission->program.size()) {
            FailMission("instruction pointer %d ran off the end of a %zu-instruction program",
                       gIp, gMission->program.size());
            return;
        }
        const Instruction& ins = gMission->program[gIp];
        auto sit = gStatements.find(ins.opcode);
        if (sit != gStatements.end()) {
            bool advance = sit->second(ins);
            if (advance) gIp++;
            if (gYield) { gYield = false; return; }
            continue;
        }
        auto cit = gConditions.find(ins.opcode);
        if (cit != gConditions.end()) {
            // A condition instruction reached directly (not consumed by a preceding IF) - III/VC
            // scripts do not do this, but if one somehow did, its own truth becomes the running
            // GOTO_IF_FALSE condition and execution continues.
            gLastCondition = ins.negate ? !cit->second(ins) : cit->second(ins);
            gIp++;
            continue;
        }
        if (IsCosmeticNoOp(ins.opcode)) {
            if (gWarnedCosmetic.insert(ins.opcode).second)
                Log::Write("missions: %s - %s treated as a cosmetic no-op (camera/audio/AI-flavour, "
                          "not verified against an address - see docs/missions.md)",
                          gMission->id.c_str(), ins.opcode.c_str());
            gIp++;
            continue;
        }
        FailMission("unsupported opcode %s", ins.opcode.c_str());
        return;
    }
    Log::Write("missions: %s - %d instructions executed with no WAIT; yielding this frame "
              "anyway to avoid stalling it (possible interpreter bug)", gMission->id.c_str(),
              kMaxStepsPerFrame);
}

const std::vector<std::string>& LoadedIds() { return gIds; }

bool Running() { return gPhase == Phase::Running && gMission; }

void Describe(std::string& out) {
    char buf[400];
    if (gPhase == Phase::Idle || !gMission) {
        snprintf(buf, sizeof(buf), "missions: idle, %zu loaded", gMissions.size());
        out += buf;
        return;
    }
    const char* phaseName = gPhase == Phase::Running ? "running"
                           : gPhase == Phase::Complete ? "complete" : "failed";
    snprintf(buf, sizeof(buf), "missions: %s (%s) - %s, instruction %d/%zu\n  last message: %s",
            gMission->id.c_str(), gMission->title.c_str(), phaseName, gIp,
            gMission->program.size(), gLastMessage.c_str());
    out += buf;
}

}  // namespace MissionEngine
