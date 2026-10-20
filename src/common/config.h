// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <string>
#include <unordered_map>

namespace avr {

// Simple key = value config file. '#' or ';' start comments. Keys are case-insensitive.
class Config {
public:
    bool Load(const wchar_t* path);
    std::string GetStr(const char* key, const char* def) const;
    int GetInt(const char* key, int def) const;
    float GetFloat(const char* key, float def) const;
    bool GetBool(const char* key, bool def) const;
    void Dump() const; // to log
private:
    std::unordered_map<std::string, std::string> m_;
};

// Writes a default config.txt if it does not exist.
void WriteDefaultConfig(const wchar_t* path);

} // namespace avr
