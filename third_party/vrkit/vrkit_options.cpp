#include "vrkit_options.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>

namespace vrkit {

static void DefaultLog(const char* m) { OutputDebugStringA(m); OutputDebugStringA("\n"); }
static void (*g_log)(const char*) = DefaultLog;
void SetLog(void (*fn)(const char*)) { g_log = fn ? fn : DefaultLog; }
void Log(const char* fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    g_log(buf);
}

Options& Options::Get() { static Options o; return o; }

Option& Options::Add(Option o) {
    o.defaultText = ValueText(o);
    m_opts.push_back(std::move(o));
    return m_opts.back();
}
Option& Options::AddBool(const char* page, const char* key, const char* label, bool* v, const char* help, std::function<void()> cb) {
    Option o; o.page = page; o.key = key; o.label = label; o.help = help; o.type = OptType::Bool; o.b = v; o.onChange = cb; return Add(o);
}
Option& Options::AddInt(const char* page, const char* key, const char* label, int* v, int mn, int mx, int st, const char* help, std::function<void()> cb) {
    Option o; o.page = page; o.key = key; o.label = label; o.help = help; o.type = OptType::Int; o.i = v; o.minV = (float)mn; o.maxV = (float)mx; o.step = (float)st; o.onChange = cb; return Add(o);
}
Option& Options::AddFloat(const char* page, const char* key, const char* label, float* v, float mn, float mx, float st, int dec, const char* help, std::function<void()> cb) {
    Option o; o.page = page; o.key = key; o.label = label; o.help = help; o.type = OptType::Float; o.f = v; o.minV = mn; o.maxV = mx; o.step = st; o.decimals = dec; o.onChange = cb; return Add(o);
}
Option& Options::AddChoice(const char* page, const char* key, const char* label, std::string* v, std::vector<std::string> choices, const char* help, std::function<void()> cb) {
    Option o; o.page = page; o.key = key; o.label = label; o.help = help; o.type = OptType::Choice; o.s = v; o.choices = std::move(choices); o.onChange = cb; return Add(o);
}
Option& Options::AddAction(const char* page, const char* label, std::function<void()> run, const char* help) {
    Option o; o.page = page; o.label = label; o.help = help; o.type = OptType::Action; o.onChange = run; return Add(o);
}

std::vector<std::string> Options::Pages() const {
    std::vector<std::string> p;
    for (auto& o : m_opts) if (std::find(p.begin(), p.end(), o.page) == p.end()) p.push_back(o.page);
    return p;
}

void Options::Changed(Option& o) {
    if (!o.key.empty()) m_dirty = true;
    if (o.onChange) o.onChange();
}

void Options::Step(Option& o, int dir) {
    switch (o.type) {
    case OptType::Bool: *o.b = !*o.b; break;
    case OptType::Int: { int v = *o.i + dir * (int)o.step; v = (std::max)((int)o.minV, (std::min)((int)o.maxV, v)); if (v == *o.i) return; *o.i = v; break; }
    case OptType::Float: {
        float v = *o.f + dir * o.step;
        float q = powf(10.f, (float)o.decimals); v = roundf(v * q) / q;            // keep values tidy (0.75, not 0.7500001)
        v = (std::max)(o.minV, (std::min)(o.maxV, v));
        if (v == *o.f) return;
        *o.f = v; break;
    }
    case OptType::Choice: {
        if (o.choices.empty()) return;
        auto it = std::find(o.choices.begin(), o.choices.end(), *o.s);
        int n = (int)o.choices.size(), k = it == o.choices.end() ? 0 : (int)(it - o.choices.begin());
        k = ((k + dir) % n + n) % n;
        *o.s = o.choices[k]; break;
    }
    case OptType::Action: return;
    }
    Changed(o);
}
void Options::Activate(Option& o) {
    if (o.type == OptType::Action) { if (o.onChange) o.onChange(); return; }
    Step(o, +1);   // bool: toggle, choice: next, number: one step up
}
void Options::Reset(Option& o) {
    if (o.type == OptType::Action || ValueText(o) == o.defaultText) return;
    switch (o.type) {
    case OptType::Bool: *o.b = o.defaultText == "true"; break;
    case OptType::Int: *o.i = atoi(o.defaultText.c_str()); break;
    case OptType::Float: *o.f = (float)atof(o.defaultText.c_str()); break;
    case OptType::Choice: *o.s = o.defaultText; break;
    default: break;
    }
    Changed(o);
}

std::string Options::ValueText(const Option& o) const {
    char b[64];
    switch (o.type) {
    case OptType::Bool: return *o.b ? "true" : "false";
    case OptType::Int: snprintf(b, sizeof(b), "%d", *o.i); return b;
    case OptType::Float: snprintf(b, sizeof(b), "%.*f", o.decimals, *o.f); return b;
    case OptType::Choice: return *o.s;
    default: return "";
    }
}
std::string Options::DisplayText(const Option& o) const {
    switch (o.type) {
    case OptType::Bool: return *o.b ? "On" : "Off";
    case OptType::Choice: {
        for (size_t k = 0; k < o.choices.size(); ++k)
            if (o.choices[k] == *o.s) return k < o.choiceLabels.size() ? o.choiceLabels[k] : o.choices[k];
        return *o.s;
    }
    case OptType::Action: return "";
    default: return ValueText(o);
    }
}

bool Options::Save(const std::wstring& path) {
    std::string all;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096]; size_t k;
        while ((k = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, k);
        fclose(f);
    }
    bool crlf = all.find("\r\n") != std::string::npos || all.empty();
    std::vector<bool> written(m_opts.size(), false);
    std::string out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t e = all.find('\n', pos); if (e == std::string::npos) e = all.size();
        std::string line = all.substr(pos, e - pos);
        bool cr = !line.empty() && line.back() == '\r'; if (cr) line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        size_t eq = line.find('=');
        if (a != std::string::npos && line[a] != '#' && eq != std::string::npos) {
            std::string key = line.substr(a, eq - a);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            for (size_t k = 0; k < m_opts.size(); ++k)
                if (!m_opts[k].key.empty() && m_opts[k].key == key) { line = key + " = " + ValueText(m_opts[k]); written[k] = true; break; }
        }
        out += line; if (cr || (crlf && e < all.size())) out += "\r"; if (e < all.size()) out += "\n";
        pos = e + 1;
    }
    std::string nl = crlf ? "\r\n" : "\n";
    bool header = false;
    for (size_t k = 0; k < m_opts.size(); ++k) {
        if (written[k] || m_opts[k].key.empty()) continue;
        if (!header) { if (!out.empty() && out.back() != '\n') out += nl; out += nl + "# --- set from the in-game menu ---" + nl; header = true; }
        out += m_opts[k].key + " = " + ValueText(m_opts[k]) + nl;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { Log("vrkit: cannot write config"); return false; }
    fwrite(out.data(), 1, out.size(), f); fclose(f);
    m_dirty = false;
    return true;
}

} // namespace vrkit
