// Parses tools/import_missions.py's own output - missions.dat (the index) and each mission's own
// <id>.dat - into the small bytecode program MissionEngine steps through. No JSON library: the
// format is one instruction per `P` line, `<kind>:<value>` operand tokens, exactly what
// serialize_program() in tools/import_missions.py writes. Lines starting `#` (including every
// `# OBJ ...` documentation line the tool also writes) are comments and skipped.
#include "MissionData.h"
#include "Log.h"
#include <fstream>
#include <sstream>

namespace MissionData {
namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Operand ParseOperand(const std::string& tok) {
    Operand op;
    if (tok.size() < 2 || tok[1] != ':') { op.kind = OperandKind::Int; op.i = 0; return op; }
    char kind = tok[0];
    std::string v = tok.substr(2);
    switch (kind) {
    case 'i': op.kind = OperandKind::Int; op.i = atoi(v.c_str()); break;
    case 'f': op.kind = OperandKind::Float; op.f = (float)atof(v.c_str()); break;
    case 'g': op.kind = OperandKind::GlobalVar; op.i = atoi(v.c_str()); break;
    case 'l': op.kind = OperandKind::LocalVar; op.i = atoi(v.c_str()); break;
    case 's': op.kind = OperandKind::Text; op.text = v; break;
    case 'L': op.kind = OperandKind::Label; op.label = atoi(v.c_str()); break;
    case 'X': op.kind = OperandKind::ExternalLabel;
        op.externalAddr = (unsigned)strtoul(v.c_str(), nullptr, 0); break;
    case 'm': op.kind = OperandKind::ModelName; op.text = v; op.i = -1; break;
    case 'u': op.kind = OperandKind::ModelName; op.text.clear(); op.i = -1;
        op.f = (float)atoi(v.c_str());   // the donor's own raw id, unresolved - kept for the log only
        break;
    default: op.kind = OperandKind::Int; op.i = 0; break;
    }
    return op;
}

// "P <index> <opcode hex> <negate> <NAME> tok tok ..."
bool ParseProgramLine(const std::string& line, Instruction& out) {
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    if (tag != "P") return false;
    int negate = 0;
    std::string opHex;
    iss >> out.index >> opHex >> negate >> out.opcode;
    out.negate = negate != 0;
    std::string tok;
    while (iss >> tok) out.operands.push_back(ParseOperand(tok));
    return true;
}

bool LoadMissionFile(const std::string& path, Mission& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("# OBJ message", 0) == 0) {
            // "# OBJ message at=... gxt_key=MEA1_B style=... time_ms=... text=The name's Chonks..."
            size_t k = line.find("gxt_key="), t = line.find(" text=");
            if (k != std::string::npos && t != std::string::npos) {
                std::string key = line.substr(k + 8, line.find(' ', k + 8) - (k + 8));
                std::string text = line.substr(t + 6);
                while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
                if (!key.empty() && !text.empty()) out.texts[key] = text;
            }
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("id ", 0) == 0) { out.id = Trim(line.substr(3)); continue; }
        if (line.rfind("title ", 0) == 0) { out.title = Trim(line.substr(6)); continue; }
        if (line.rfind("region ", 0) == 0) { out.region = Trim(line.substr(7)); continue; }
        if (line.rfind("scm_range ", 0) == 0) {
            std::istringstream iss(line.substr(10));
            iss >> out.scmRangeLo >> out.scmRangeHi;
            continue;
        }
        if (line.rfind("phone_pos ", 0) == 0) {
            std::istringstream iss(line.substr(10));
            iss >> out.phoneX >> out.phoneY >> out.phoneZ;
            out.hasPhonePos = true;
            continue;
        }
        Instruction ins;
        if (ParseProgramLine(line, ins)) out.program.push_back(std::move(ins));
    }
    return !out.id.empty() && !out.program.empty();
}

}  // namespace

std::map<std::string, Mission> LoadFolder(const std::string& folder) {
    std::map<std::string, Mission> out;
    if (folder.empty()) return out;
    std::ifstream index(folder + "\\missions.dat");
    if (!index) {
        Log::Write("missions: no missions.dat in %s - run tools/import_missions.py and copy its "
                   "output here (see docs/missions.md); nothing to load", folder.c_str());
        return out;
    }
    std::string line;
    int wanted = 0, loaded = 0;
    while (std::getline(index, line)) {
        if (line.rfind("MISSION ", 0) != 0) continue;
        std::istringstream iss(line.substr(8));
        std::string id, file;
        iss >> id >> file;
        if (id.empty() || file.empty()) continue;
        wanted++;
        Mission m;
        if (LoadMissionFile(folder + "\\" + file, m)) {
            loaded++;
            out[id] = std::move(m);
        } else {
            Log::Write("missions: %s (%s) did not parse - skipped", id.c_str(), file.c_str());
        }
    }
    Log::Write("missions: %d/%d mission(s) loaded from %s", loaded, wanted, folder.c_str());
    return out;
}

}  // namespace MissionData
