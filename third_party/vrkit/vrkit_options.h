// vrkit - reusable pieces for VR game mods (D3D11 + OpenXR).
// Option registry: one list of user settings that the in-game menu shows and edits, and that is written
// back to the mod's key = value config file. The host keeps its own parser and settings struct; it
// registers pointers to its fields here, with an optional callback that applies a change live.
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace vrkit {

enum class OptType { Bool, Int, Float, Choice, Action };

struct Option {
    std::string page;          // menu page / section ("Display", "Foveation", ...)
    std::string key;           // config key (empty for actions: not saved)
    std::string label;         // text shown in the menu
    std::string help;          // one or two lines shown at the bottom when selected
    OptType type = OptType::Bool;
    bool* b = nullptr; int* i = nullptr; float* f = nullptr; std::string* s = nullptr;
    float minV = 0, maxV = 1, step = 0.1f;   // Int/Float
    int decimals = 2;                         // Float: digits shown and written
    std::vector<std::string> choices;         // Choice: values as written in the config
    std::vector<std::string> choiceLabels;    // Choice: optional display names (same order)
    bool needsRestart = false;                // shown as "(restart)"; the value is still saved
    std::function<void()> onChange;           // apply live (called after every change)
    std::function<bool()> visible;            // optional: hide when this returns false
    std::string defaultText;                  // value at registration, for "reset"
};

class Options {
public:
    static Options& Get();
    // Registration (returns the option so callers can set extra fields). Pages appear in first-use order.
    Option& AddBool(const char* page, const char* key, const char* label, bool* v, const char* help = "", std::function<void()> onChange = {});
    Option& AddInt(const char* page, const char* key, const char* label, int* v, int minV, int maxV, int step, const char* help = "", std::function<void()> onChange = {});
    Option& AddFloat(const char* page, const char* key, const char* label, float* v, float minV, float maxV, float step, int decimals, const char* help = "", std::function<void()> onChange = {});
    Option& AddChoice(const char* page, const char* key, const char* label, std::string* v, std::vector<std::string> choices, const char* help = "", std::function<void()> onChange = {});
    Option& AddAction(const char* page, const char* label, std::function<void()> run, const char* help = "");

    std::vector<Option>& All() { return m_opts; }
    std::vector<std::string> Pages() const;

    // Editing (used by the menu). dir = -1 / +1 (left / right), Activate = A / Enter.
    void Step(Option& o, int dir);
    void Activate(Option& o);
    void Reset(Option& o);
    std::string ValueText(const Option& o) const;    // as written in the config
    std::string DisplayText(const Option& o) const;  // as shown in the menu
    bool Dirty() const { return m_dirty; }

    // Write every registered option's current value into the config file: existing "key = value" lines
    // are updated in place (comments and order kept); missing keys are appended at the end.
    bool Save(const std::wstring& path);

private:
    Option& Add(Option o);
    void Changed(Option& o);
    std::vector<Option> m_opts;
    bool m_dirty = false;
};

// Logging hook (set by the host; default: OutputDebugString).
void SetLog(void (*fn)(const char* msg));
void Log(const char* fmt, ...);

} // namespace vrkit
