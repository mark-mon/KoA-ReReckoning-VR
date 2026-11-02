#include "vrkit_menu_core.h"
#include "vrkit_options.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include "imgui.h"

namespace vrkit { namespace menu { namespace core {

namespace {
Settings g_s;
ImGuiContext* g_im = nullptr;
bool g_open = false;
int g_page = 0, g_sel = 0;
LARGE_INTEGER g_qpf, g_last;

std::vector<int> PageItems(int page) {
    auto& opts = Options::Get().All();
    auto pages = Options::Get().Pages();
    std::vector<int> r;
    if (pages.empty()) return r;
    page = (std::min)(page, (int)pages.size() - 1);
    for (int k = 0; k < (int)opts.size(); ++k)
        if (opts[k].page == pages[page] && (!opts[k].visible || opts[k].visible())) r.push_back(k);
    return r;
}

void Close() {
    g_open = false;
    input::SetBlocking(false);
    if (Options::Get().Dirty() && !g_s.configPath.empty()) {
        if (Options::Get().Save(g_s.configPath)) Log("vrkit: settings saved");
    }
}

void Navigate(uint32_t b) {
    auto pages = Options::Get().Pages();
    if (pages.empty()) return;
    if (b & input::PagePrev) { g_page = (g_page + (int)pages.size() - 1) % (int)pages.size(); g_sel = 0; }
    if (b & input::PageNext) { g_page = (g_page + 1) % (int)pages.size(); g_sel = 0; }
    auto items = PageItems(g_page);
    if (items.empty()) return;
    int n = (int)items.size();
    if (b & input::Up) g_sel = (g_sel + n - 1) % n;
    if (b & input::Down) g_sel = (g_sel + 1) % n;
    g_sel = (std::min)(g_sel, n - 1);
    Option& o = Options::Get().All()[items[g_sel]];
    if (b & input::Left) Options::Get().Step(o, -1);
    if (b & input::Right) Options::Get().Step(o, +1);
    if (b & input::Accept) Options::Get().Activate(o);
    if (b & input::Reset) Options::Get().Reset(o);
}
} // namespace

bool InitImGui(const Settings& s) {
    if (g_im) return true;
    g_s = s;
    input::Configure(s.input);
    input::InstallHooks();
    IMGUI_CHECKVERSION();
    g_im = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)s.texW, (float)s.texH);
    // a clear UI font from Windows at the requested size; ImGui's built-in font as fallback
    float os = s.fontOversample > 0 ? s.fontOversample : 1.f;
    ImFont* f = nullptr;
    wchar_t win[MAX_PATH]; char path[MAX_PATH * 2];
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
        for (const wchar_t* name : { L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf" }) {
            std::wstring p = std::wstring(win) + name;
            WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, path, sizeof(path), nullptr, nullptr);
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && (f = io.Fonts->AddFontFromFileTTF(path, s.fontPx * os))) break;
        }
    }
    if (!f) { ImFontConfig fc; fc.SizePixels = s.fontPx * os; io.Fonts->AddFontDefault(&fc); }
    io.FontGlobalScale = 1.f / os;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.f; st.WindowBorderSize = 0.f; st.WindowPadding = ImVec2(0, 0);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, s.backgroundAlpha);
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_last);
    return true;
}

void ShutdownImGui() {
    if (g_im) { ImGui::DestroyContext(g_im); g_im = nullptr; }
    g_open = false;
}

const Settings& Get() { return g_s; }

bool UpdateInput() {
    if (!g_im) return false;
    static int rehook = 0;
    if ((++rehook % 300) == 1) input::InstallHooks();   // the game may load XInput late
    uint32_t b = input::Poll();
    if (b & input::Toggle) { if (g_open) Close(); else { g_open = true; input::SetBlocking(true); } b &= ~input::Toggle; }
    else if (g_open && (b & input::Back)) { Close(); return false; }
    if (!g_open) return false;
    Navigate(b);
    ImGui::SetCurrentContext(g_im);
    return true;
}

void BuildFrame() {
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = (std::max)(0.001f, (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart); g_last = now;
    io.DisplaySize = ImVec2((float)g_s.texW, (float)g_s.texH);
    ImGui::NewFrame();
    const float W = (float)g_s.texW, H = (float)g_s.texH, pad = g_s.fontPx * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##vrkit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    const ImU32 cText = IM_COL32(235, 238, 245, 255), cDim = IM_COL32(150, 158, 175, 255), cAcc = IM_COL32(110, 180, 255, 255),
                cSel = IM_COL32(60, 90, 140, 255), cWarn = IM_COL32(255, 190, 90, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = pad;
    // title + status
    dl->AddText(ImVec2(pad, y), cText, g_s.title.c_str());
    if (g_s.statusLine) { std::string st = g_s.statusLine(); ImVec2 ts = ImGui::CalcTextSize(st.c_str()); dl->AddText(ImVec2(W - pad - ts.x, y), cDim, st.c_str()); }
    y += g_s.fontPx * 1.4f;
    // page tabs
    auto pages = Options::Get().Pages();
    float x = pad;
    for (int p = 0; p < (int)pages.size(); ++p) {
        ImVec2 ts = ImGui::CalcTextSize(pages[p].c_str());
        if (p == g_page) dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + ts.x + 8, y + ts.y + 4), cSel, 6.f);
        dl->AddText(ImVec2(x, y), p == g_page ? cText : cDim, pages[p].c_str());
        x += ts.x + 28;
    }
    y += g_s.fontPx * 1.5f;
    dl->AddLine(ImVec2(pad, y - 8), ImVec2(W - pad, y - 8), cDim, 2.f);
    // rows (scrolled so the selection stays visible)
    auto items = PageItems(g_page);
    float rowH = g_s.fontPx * 1.35f, listTop = y, listBottom = H - g_s.fontPx * 5.2f;
    int visibleRows = (std::max)(1, (int)((listBottom - listTop) / rowH));
    int first = (std::max)(0, (std::min)(g_sel - visibleRows / 2, (int)items.size() - visibleRows));
    auto& opts = Options::Get().All();
    for (int r = first; r < (int)items.size() && r < first + visibleRows; ++r) {
        Option& o = opts[items[r]];
        float ry = listTop + (r - first) * rowH;
        if (r == g_sel) dl->AddRectFilled(ImVec2(pad * 0.5f, ry - 4), ImVec2(W - pad * 0.5f, ry + rowH - 6), cSel, 6.f);
        dl->AddText(ImVec2(pad, ry), cText, o.label.c_str());
        std::string v = Options::Get().DisplayText(o);
        bool adjustable = o.type != OptType::Action;
        std::string shown = o.type == OptType::Action ? "[ run ]" : (r == g_sel && adjustable ? "<  " + v + "  >" : v);
        ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
        float vx = W - pad - vs.x;
        if (o.needsRestart) {
            const char* rs = "restart";
            ImVec2 rsz = ImGui::CalcTextSize(rs);
            dl->AddText(ImVec2(vx - rsz.x - 24, ry), cWarn, rs);
        }
        dl->AddText(ImVec2(vx, ry), r == g_sel ? cAcc : cText, shown.c_str());
    }
    if (items.empty()) dl->AddText(ImVec2(pad, listTop), cDim, "(nothing on this page)");
    // help for the selected option + controls
    float hy = H - g_s.fontPx * 4.6f;
    dl->AddLine(ImVec2(pad, hy - 10), ImVec2(W - pad, hy - 10), cDim, 2.f);
    if (!items.empty()) {
        const Option& o = opts[items[(std::min)(g_sel, (int)items.size() - 1)]];
        std::string h = o.help;
        if (!o.key.empty()) h += (h.empty() ? "" : "   ") + std::string("[") + o.key + "]";
        ImGui::SetCursorScreenPos(ImVec2(pad, hy));
        ImGui::PushTextWrapPos(W - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, cDim);
        ImGui::TextWrapped("%s", h.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    }
    const char* keys = "A: change   Y: undo   LB/RB: page   B: close";
    dl->AddText(ImVec2(pad, H - g_s.fontPx * 1.4f), cDim, keys);
    ImGui::End();
    ImGui::Render();
}

bool IsOpen() { return g_open; }
void SetOpen(bool open) { if (open == g_open) return; if (open) { g_open = true; input::SetBlocking(true); } else Close(); }

} } } // namespace vrkit::menu::core
