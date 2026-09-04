#pragma once
#include <map>
#include <string>
#include <vector>

class IniFile {
public:
    struct Section {
        std::string name;
        std::map<std::string, std::string> values;

        bool Has(const std::string& key) const;
        std::string GetString(const std::string& key, const std::string& def) const;
        float GetFloat(const std::string& key, float def) const;
        int GetInt(const std::string& key, int def) const;
        bool GetBool(const std::string& key, bool def) const;
    };

    bool Load(const std::string& path);
    const Section* Find(const std::string& name) const;
    const std::vector<Section>& Sections() const { return sections; }

private:
    std::vector<Section> sections;
};
