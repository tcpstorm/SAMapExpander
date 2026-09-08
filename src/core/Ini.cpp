#include "Ini.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static std::string Unquote(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

bool IniFile::Section::Has(const std::string& key) const {
    return values.count(Lower(key)) != 0;
}

std::string IniFile::Section::GetString(const std::string& key, const std::string& def) const {
    auto it = values.find(Lower(key));
    return it == values.end() ? def : it->second;
}

float IniFile::Section::GetFloat(const std::string& key, float def) const {
    auto it = values.find(Lower(key));
    if (it == values.end()) return def;
    char* end = nullptr;
    float v = strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? def : v;
}

int IniFile::Section::GetInt(const std::string& key, int def) const {
    auto it = values.find(Lower(key));
    if (it == values.end()) return def;
    char* end = nullptr;
    long v = strtol(it->second.c_str(), &end, 0);
    return end == it->second.c_str() ? def : (int)v;
}

bool IniFile::Section::GetBool(const std::string& key, bool def) const {
    auto it = values.find(Lower(key));
    if (it == values.end()) return def;
    std::string v = Lower(it->second);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

bool IniFile::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    sections.clear();
    sections.push_back({ "", {} });

    std::string line;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            sections.push_back({ Trim(line.substr(1, line.size() - 2)), {} });
            continue;
        }

        size_t sep = line.find('=');
        if (sep == std::string::npos) sep = line.find_first_of(" \t");
        if (sep == std::string::npos) continue;

        std::string key = Lower(Trim(line.substr(0, sep)));
        std::string value = Unquote(Trim(line.substr(sep + 1)));
        sections.back().values[key] = value;
    }
    return true;
}

const IniFile::Section* IniFile::Find(const std::string& name) const {
    std::string want = Lower(name);
    for (auto& s : sections)
        if (Lower(s.name) == want) return &s;
    return nullptr;
}
