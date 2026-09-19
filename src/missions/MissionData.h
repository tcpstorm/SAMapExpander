#pragma once
#include <string>
#include <vector>
#include <map>

// The in-memory form of one mission mined by tools/import_missions.py, read from its <id>.dat -
// see docs/missions.md for the file format (serialize_program() in that tool writes exactly
// what this reads) and MissionEngine.cpp for what runs it.
//
// This is a small bytecode program, not a flattened list of things to do: the donor script's own
// control flow (WAIT, IF/GOTO_IF_FALSE, GOTO, variables) is kept, because a mission is a sequence
// - brief, then drive, then the twist - and nothing that throws control flow away can play one.
namespace MissionData {

enum class OperandKind { Int, Float, GlobalVar, LocalVar, Text, Label, ExternalLabel, ModelName };

struct Operand {
    OperandKind kind = OperandKind::Int;
    int i = 0;
    float f = 0.0f;
    std::string text;
    int label = -1;            // OperandKind::Label: index into the SAME mission's program
    unsigned externalAddr = 0; // OperandKind::ExternalLabel: a donor address this tool never mined
    // OperandKind::ModelName: `text` is the region-namespaced name (e.g. "v_ambulan") to resolve
    // via CModelInfo::GetModelInfo at mission load, or empty if tools/import_missions.py could
    // not map the donor's own model id to one at all (`i` then carries that raw donor id, purely
    // for the log) - either way `i` is filled in with the resolved SA model id once
    // MissionEngine's load-time resolution pass has run, and stays -1 until then.
};

struct Instruction {
    int index = 0;
    std::string opcode;       // e.g. "WAIT", "CREATE_CHAR", "IF", "GOTO_IF_FALSE"
    bool negate = false;      // the donor bytecode's own "NOT" flag on this instruction's result
    std::vector<Operand> operands;
};

struct Mission {
    std::string id;
    std::string title;
    std::string region;
    std::string scmRangeLo, scmRangeHi;
    std::vector<Instruction> program;
    // The mission's own real staging position, mined from its own SET_PLAYER_COORDINATES - see
    // tools/import_missions.py's mine_mission. In the donor's own coordinate system; the region
    // origin is added once by the engine, the same convention restarts.dat/parkedcars.dat use.
    bool hasPhonePos = false;
    float phoneX = 0.0f, phoneY = 0.0f, phoneZ = 0.0f;
    // GXT key -> the donor's own line, from the tool's "# OBJ message ... gxt_key=K ... text=..."
    // comment lines. A PRINT's operand is the key; the engine shows this text for it.
    std::map<std::string, std::string> texts;
};

// Every mission whose <id>.dat parsed cleanly, keyed by id, read from `folder`'s missions.dat
// index and the per-mission files it names. A mission the index names but whose file is missing
// or malformed is skipped and logged - never crashes the load.
std::map<std::string, Mission> LoadFolder(const std::string& folder);

}  // namespace MissionData
