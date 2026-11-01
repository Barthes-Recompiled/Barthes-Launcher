// barthes_launcher.exe: one place for the Barthes Recompiled Harry Potter ports. The games stand as books on a shelf
// at the left; the chosen one's page at the right installs it from its GitHub releases, brings in the player's disc
// (an image is unpacked, a folder copied), checks the files against the disc's known-good list and starts the game
// (Game settings opens the port's settings page first). Each game has an Archipelago page.
// Dear ImGui over Direct3D 11, like the games' own launchers; the look is the Barthes Launcher's own.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "games.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef BARTHES_VERSION
#define BARTHES_VERSION "1.0.1"
#endif

namespace {

// ── palette: ink on paper ───────────────────────────────────────────────────────────────────────────────────────

ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

const ImU32 kShelf = Rgb(22, 24, 30);         // the dark wall the books stand against
const ImU32 kShelfLine = Rgb(44, 47, 56);
const ImU32 kPaper = Rgb(240, 233, 218);      // the page
const ImU32 kPaperEdge = Rgb(222, 212, 192);
const ImU32 kInk = Rgb(38, 33, 30);           // text on the page
const ImU32 kInkSoft = Rgb(110, 100, 90);     // secondary text
const ImU32 kInkFaint = Rgb(160, 150, 136);
const ImU32 kGood = Rgb(56, 112, 74);
const ImU32 kWarn = Rgb(170, 110, 30);
const ImU32 kBad = Rgb(160, 52, 44);

// a game's accent from its hue: deep, slightly dusty, like cloth bindings
ImU32 Binding(float hue, float v = 0.46f, float s = 0.55f) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
}

// ── window and device ───────────────────────────────────────────────────────────────────────────────────────────

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_minimized = false;
bool g_refresh = false;  // the window came back to the front: folders may have changed meanwhile

void MakeRtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr))  // no usable GPU driver: WARP still draws a launcher
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    MakeRtv();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        g_minimized = wp == SIZE_MINIMIZED;
        if (g_dev && !g_minimized) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            MakeRtv();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) g_refresh = true;
        break;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd);
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {LONG(860 * (dpi ? dpi : 96) / 96), LONG(560 * (dpi ? dpi : 96) / 96)};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── fonts: Windows' own serif and sans, so nothing is shipped ───────────────────────────────────────────────────

ImFont* g_body = nullptr;
ImFont* g_small = nullptr;
ImFont* g_title = nullptr;   // the page heading
ImFont* g_spine = nullptr;   // titles up the spines
ImFont* g_italic = nullptr;  // the quote
ImFont* g_numeral = nullptr;

ImFont* LoadFont(const char* file, float size, const char* fallback = nullptr) {
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    ImGuiIO& io = ImGui::GetIO();
    for (const char* f : {file, fallback}) {
        if (!f) continue;
        std::string path = std::string(windir) + "\\Fonts\\" + f;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2160, 0x216F, 0};
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, nullptr, ranges)) return font;
    }
    ImFontConfig cfg;
    cfg.SizePixels = size;
    return io.Fonts->AddFontDefault(&cfg);
}

void LoadFonts(float sc) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_body = LoadFont("segoeui.ttf", 17 * sc, "arial.ttf");
    g_small = LoadFont("segoeui.ttf", 14 * sc, "arial.ttf");
    g_title = LoadFont("georgia.ttf", 34 * sc, "times.ttf");
    g_spine = LoadFont("georgia.ttf", 21 * sc, "times.ttf");
    g_italic = LoadFont("georgiai.ttf", 15 * sc, "timesi.ttf");
    g_numeral = LoadFont("georgiab.ttf", 22 * sc, "timesbd.ttf");
    io.FontDefault = g_body;
}

// ── drawing helpers ─────────────────────────────────────────────────────────────────────────────────────────────

// text turned a quarter left, reading upward (as on most spines in Europe), centred on c
void TextUp(ImDrawList* dl, ImFont* f, ImVec2 c, ImU32 col, const char* text) {
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, text);
    int first = dl->VtxBuffer.Size;
    // clipping happens before the turn, so it must not cut the text where it lies flat
    dl->PushClipRect(ImVec2(-1e5f, -1e5f), ImVec2(1e5f, 1e5f), false);
    dl->AddText(f, f->FontSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
    dl->PopClipRect();
    for (int k = first; k < dl->VtxBuffer.Size; k++) {
        ImVec2& p = dl->VtxBuffer[k].pos;
        float dx = p.x - c.x, dy = p.y - c.y;
        p = ImVec2(c.x + dy, c.y - dx);
    }
}

void PushPaperStyle() {
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInk));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, V(kInkFaint));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V(Rgb(250, 246, 237)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, V(Rgb(255, 252, 245)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, V(Rgb(255, 252, 245)));
    ImGui::PushStyleColor(ImGuiCol_Border, V(kPaperEdge));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, V(kInk));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, V(Rgb(250, 246, 237)));
    ImGui::PushStyleColor(ImGuiCol_Header, V(Rgb(226, 216, 196)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V(Rgb(232, 223, 205)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, V(Rgb(220, 208, 186)));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, V(kInk));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, V(Rgb(214, 196, 160)));
}
void PopPaperStyle() { ImGui::PopStyleColor(13); }

// the main action on a page: solid in the game's binding colour
bool InkButton(const char* label, ImVec2 size, ImU32 color, bool enabled = true) {
    ImVec4 c = V(color);
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 1.15f, c.y * 1.15f, c.z * 1.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kPaper));
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    return r;
}

// the rest: ruled outline, no fill
bool RuleButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(Rgb(38, 33, 30, 22)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V(Rgb(38, 33, 30, 40)));
    ImGui::PushStyleColor(ImGuiCol_Border, V(Rgb(38, 33, 30, 120)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// a mark and a line: the state of one thing
void StateLine(ImU32 mark, const char* label, const std::string& text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + h * 0.3f, p.y + h * 0.55f), h * 0.2f, mark);
    ImGui::SetCursorScreenPos(ImVec2(p.x + h * 0.9f, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(p.x - ImGui::GetWindowPos().x + h * 6.2f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

void Rule(float sc) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 6 * sc), ImVec2(p.x + w, p.y + 6 * sc), kPaperEdge, 1.0f * sc);
    ImGui::Dummy(ImVec2(w, 12 * sc));
}

void Heading(const char* text) {
    ImGui::PushFont(g_small);
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    std::string up;
    for (const char* c = text; *c; c++) { up += char(toupper((unsigned char)*c)); up += ' '; }
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void OpenUrl(const char* url) { ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

bool Link(const char* label, const char* url) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(Rgb(70, 60, 120)));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, Rgb(70, 60, 120), 1.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (hovered && ImGui::IsMouseClicked(0)) { OpenUrl(url); return true; }
    return false;
}

// ── file pickers ────────────────────────────────────────────────────────────────────────────────────────────────

bool PickFile(std::wstring& out, bool folder, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dlg->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC types[] = {
            {L"Xbox disc image or game folder", L"*.iso;*.xiso;default.xbe"},
            {L"Disc image (*.iso, *.xiso)", L"*.iso;*.xiso"},
            {L"Game folder (default.xbe)", L"default.xbe"},
        };
        dlg->SetFileTypes(3, types);
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ── the pages ───────────────────────────────────────────────────────────────────────────────────────────────────

const char* const kNumerals[] = {"I", "II", "III"};
int g_sel = 0;                 // the book pulled out; games_Count() is the colophon
int g_tab = 0;                 // 0 play, 1 Archipelago
bool g_install_when_known = false;  // --install: once the release check is in
float g_pull[8] = {};          // how far each book sits out of the shelf, eased
std::string g_message;         // the last thing that went wrong (or right), under the page's actions
bool g_message_bad = false;

void Say(const std::string& m, bool bad) {
    g_message = m;
    g_message_bad = bad;
}

void DrawShelf(ImDrawList* dl, ImVec2 a, ImVec2 b, float sc) {
    dl->AddRectFilled(a, b, kShelf);
    // the launcher's name across the top of the shelf
    ImGui::PushFont(g_small);
    dl->AddText(ImVec2(a.x + 22 * sc, a.y + 20 * sc), Rgb(200, 190, 170), "BARTHES");
    dl->AddText(ImVec2(a.x + 22 * sc, a.y + 38 * sc), Rgb(120, 115, 108), "RECOMPILED");
    ImGui::PopFont();

    int n = games_Count();
    float spine_w = 62 * sc, gap = 10 * sc;
    float floor_y = b.y - 70 * sc, top_y = a.y + 86 * sc;
    float x = a.x + 22 * sc;
    dl->AddLine(ImVec2(a.x + 12 * sc, floor_y + 1), ImVec2(b.x - 12 * sc, floor_y + 1), kShelfLine, 3 * sc);
    for (int i = 0; i <= n; i++) {
        bool colophon = i == n;
        // books of slightly different heights, the colophon a thin pamphlet
        float h = colophon ? (floor_y - top_y) * 0.52f : (floor_y - top_y) * (0.92f + 0.04f * float((i * 7) % 3) - 0.04f);
        float w = colophon ? spine_w * 0.55f : spine_w;
        float target = g_sel == i ? 1.0f : 0.0f;
        g_pull[i] += (target - g_pull[i]) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
        float lift = g_pull[i] * 14 * sc;
        ImVec2 p0(x, floor_y - h - lift), p1(x + w, floor_y - lift);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("book", ImVec2(w, h))) {
            g_sel = i;
            g_tab = 0;
            g_message.clear();
            if (!colophon) games_Refresh(i);
        }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImU32 cloth = colophon ? Rgb(196, 186, 164) : Binding(games_Info(i).hue, hov ? 0.52f : 0.46f);
        ImU32 band = colophon ? Rgb(150, 140, 120) : Binding(games_Info(i).hue, 0.30f, 0.45f);
        ImU32 letter = colophon ? Rgb(60, 52, 44) : Rgb(236, 222, 190);
        dl->AddRectFilled(ImVec2(p0.x + 3 * sc, p0.y + 3 * sc), ImVec2(p1.x + 3 * sc, p1.y), Rgb(0, 0, 0, 70), 3 * sc);
        dl->AddRectFilled(p0, p1, cloth, 3 * sc);
        // head and tail bands, and a faint highlight down the spine's curve
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 14 * sc), ImVec2(p1.x, p0.y + 18 * sc), band);
        dl->AddRectFilled(ImVec2(p0.x, p1.y - 18 * sc), ImVec2(p1.x, p1.y - 14 * sc), band);
        dl->AddRectFilledMultiColor(ImVec2(p0.x + w * 0.18f, p0.y + 2), ImVec2(p0.x + w * 0.42f, p1.y - 2),
                                    Rgb(255, 255, 255, 0), Rgb(255, 255, 255, 26), Rgb(255, 255, 255, 26), Rgb(255, 255, 255, 0));
        if (colophon) {
            TextUp(dl, g_small, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), letter, "COLOPHON");
        } else {
            ImVec2 ns = g_numeral->CalcTextSizeA(g_numeral->FontSize, FLT_MAX, 0, kNumerals[i]);
            dl->AddText(g_numeral, g_numeral->FontSize, ImVec2((p0.x + p1.x - ns.x) * 0.5f, p1.y - 52 * sc), letter, kNumerals[i]);
            TextUp(dl, g_spine, ImVec2((p0.x + p1.x) * 0.5f, p0.y + (h - 60 * sc) * 0.5f + 6 * sc), letter, games_Info(i).title);
        }
        x += w + gap;
    }
    // the quote, small, under the shelf
    ImGui::PushFont(g_italic);
    float wrap = b.x - a.x - 44 * sc;
    const char* q = "\xE2\x80\x9CTo give a text an Author is to impose a limit on that text.\xE2\x80\x9D";
    dl->AddText(g_italic, g_italic->FontSize, ImVec2(a.x + 22 * sc, floor_y + 12 * sc), Rgb(170, 160, 142), q, nullptr, wrap);
    ImGui::PopFont();
    ImGui::PushFont(g_small);
    ImVec2 qs = g_italic->CalcTextSizeA(g_italic->FontSize, FLT_MAX, wrap, q);
    dl->AddText(ImVec2(a.x + 22 * sc, floor_y + 16 * sc + qs.y), Rgb(120, 115, 108), "Roland Barthes");
    ImGui::PopFont();
}

std::string BuildLine(int i, ImU32& mark) {
    const GameState& s = games_State(i);
    if (!s.installed) {
        mark = kInkFaint;
        return "Not installed";
    }
    std::string v = s.version.empty() ? "Installed" : "Version " + s.version;
    if (games_UpdateAvailable(i)) {
        mark = kWarn;
        return v + ", " + s.release.tag + " is out";
    }
    mark = kGood;
    return v;
}

std::string DiscLine(int i, ImU32& mark) {
    const GameState& s = games_State(i);
    if (s.wrong_game) {
        mark = kBad;
        const DiscList* d = disc_Get(s.disc);
        return std::string("game\\ holds ") + games_Info(d->game[2] - '1').title + " (" + d->edition + "), not this game";
    }
    if (s.has_disc) {
        if (s.disc < 0 && !s.xbe_edition.empty()) {  // known by its default.xbe alone (no file list to verify against)
            bool other = games_BuildMismatch(i);
            mark = other ? kWarn : kGood;
            return s.xbe_edition + " disc" +
                   (other ? ", but the installed build is for the " + s.build_edition + " disc: reinstall to get the right one"
                          : "");
        }
        if (s.disc < 0) {
            mark = games_Info(i).playable ? kWarn : kInkSoft;
            return games_Info(i).playable ? "Present, but not a disc the launcher recognises" : "Present";
        }
        if (games_BuildMismatch(i)) {
            mark = kWarn;
            return std::string(disc_Get(s.disc)->edition) + " disc, but the installed build is for the " + s.build_edition +
                   " disc: reinstall to get the right one";
        }
        mark = s.verified.empty() ? kInkSoft : kGood;
        return std::string(disc_Get(s.disc)->edition) + " disc" + (s.verified.empty() ? ", not verified yet" : ", verified " + s.verified.substr(0, 10));
    }
    if (s.has_image) {
        mark = kWarn;
        return "A disc image is in game\\: choose it with Add disc image or folder to unpack it";
    }
    mark = kInkFaint;
    return "None yet: add your own disc image or game folder";
}

void DrawJob(float sc, ImU32 accent) {
    Job& j = games_Job();
    std::string line;
    float f = games_JobProgress(line);
    ImGui::TextUnformatted(line.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 6 * sc;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kPaperEdge, h);
    if (f >= 0) dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + h), accent, h);
    else {  // size unknown: a sliding mark
        float t = float(fmod(ImGui::GetTime() * 0.6, 1.0));
        dl->AddRectFilled(ImVec2(p.x + w * t * 0.8f, p.y), ImVec2(p.x + w * (t * 0.8f + 0.2f), p.y + h), accent, h);
    }
    ImGui::Dummy(ImVec2(w, h + 8 * sc));
    if (RuleButton("Cancel", ImVec2(120 * sc, 0))) games_CancelJob();
    if (j.finished) {
        Say(j.result.empty() ? (j.ok ? "Done." : "Stopped.") : j.result, !j.ok);
        games_FinishJob();
    }
}

void DrawPlayTab(int i, float sc) {
    const GameInfo& g = games_Info(i);
    GameState& s = games_State(i);
    ImU32 accent = Binding(g.hue);
    Job& job = games_Job();
    bool busy = job.running != 0;

    ImU32 mark;
    std::string build = BuildLine(i, mark);
    StateLine(mark, "Build", build);
    std::string disc = DiscLine(i, mark);
    StateLine(mark, "Disc", disc);
    StateLine(kInkFaint, "Folder", Narrow(s.folder));
    ImGui::Dummy(ImVec2(0, 10 * sc));

    if (busy && job.game == i) {
        DrawJob(sc, accent);
        return;
    }
    if (busy) {
        ImGui::TextDisabled("Another game's job is running; this page waits for it.");
        return;
    }

    bool can_play = s.installed && !s.running && !s.wrong_game && s.has_disc;
    float bw = 200 * sc, bh = 46 * sc;
    if (s.running) {
        InkButton("Playing...", ImVec2(bw, bh), accent, false);
    } else if (InkButton("Play", ImVec2(bw, bh), accent, can_play)) {
        std::string err;
        if (games_Play(i, false, err)) {
            Say("", false);
            if (games_AfterStart() == AFTER_CLOSE) PostQuitMessage(0);
            else if (games_AfterStart() == AFTER_MINIMIZE) ShowWindow(g_hwnd, SW_MINIMIZE);
        } else {
            Say(err, true);
        }
    }
    ImGui::SameLine(0, 12 * sc);
    if (RuleButton("Game settings", ImVec2(160 * sc, bh), s.installed && !s.running)) {
        std::string err;
        if (!games_Play(i, true, err)) Say(err, true);
    }
    if (!can_play && !s.running) {
        ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
        if (!g.playable && !s.installed) ImGui::TextWrapped("This port is still being built. It will appear here once it boots.");
        else if (!s.installed) ImGui::TextWrapped("Install the game first, then add your disc.");
        else if (s.wrong_game) ImGui::TextWrapped("The disc in game\\ belongs to another game. Add this game's disc.");
        else ImGui::TextWrapped("Add your disc (an .iso or an extracted game folder) to play.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 8 * sc));
    Rule(sc);

    Heading("Install");
    if (!s.release_checked) {
        ImGui::TextDisabled("Looking for releases on GitHub...");
    } else if (!s.has_release) {
        ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
        ImGui::TextWrapped("%s", s.release_error.empty() ? "No release has been published on GitHub yet." : s.release_error.c_str());
        ImGui::PopStyleColor();
    } else {
        bool mismatch = games_BuildMismatch(i);
        const char* label = !s.installed ? "Install %s" : games_UpdateAvailable(i) ? "Update to %s"
                          : mismatch ? "Reinstall %s for this disc" : "Reinstall %s";
        char buf[96];
        snprintf(buf, sizeof buf, label, s.release.tag.c_str());
        bool primary = !s.installed || games_UpdateAvailable(i) || mismatch;
        if (primary ? InkButton(buf, ImVec2(0, 0), accent, !s.running) : RuleButton(buf, ImVec2(0, 0), !s.running)) {
            if (!games_StartInstall(i)) Say("Could not start the install.", true);
        }
        ImGui::SameLine();
    }
    if (RuleButton("Use another folder...")) {
        std::wstring dir;
        if (PickFile(dir, true, L"Choose the folder with the game's launcher (or an empty folder to install into)")) {
            s.folder = dir;
            s.custom_folder = true;
            games_Save();
            games_Refresh(i);
            Say(s.installed ? "Found the game there." : "No build there yet: installing will put it in that folder.", false);
        }
    }
    if (s.custom_folder) {
        ImGui::SameLine();
        if (RuleButton("Back to the default folder")) {
            s.custom_folder = false;
            s.folder = games_ExeDir() + L"games\\" + Widen(g.id);
            games_Save();
            games_Refresh(i);
        }
    }
    ImGui::Dummy(ImVec2(0, 6 * sc));
    Heading("Disc");
    if (RuleButton("Add disc image or folder...", ImVec2(0, 0), !s.running)) {
        std::wstring src;
        if (PickFile(src, false, L"Choose your disc image (.iso), or default.xbe in an extracted game folder")) {
            std::string err;
            if (!games_StartImport(i, src, err)) Say(err, true);
            else Say("", false);
        }
    }
    ImGui::SameLine();
    bool listed = s.has_disc && s.disc >= 0 && !s.wrong_game;
    if (RuleButton("Verify files", ImVec2(0, 0), listed && !s.running)) games_StartVerify(i);
    ImGui::SameLine();
    if (RuleButton("Open folder")) games_OpenFolder(i, false);

    if (!g_message.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * sc));
        ImGui::PushStyleColor(ImGuiCol_Text, V(g_message_bad ? kBad : kGood));
        ImGui::TextWrapped("%s", g_message.c_str());
        ImGui::PopStyleColor();
    }
}

void DrawArchipelagoTab(int i, float sc) {
    const GameInfo& g = games_Info(i);
    GameState& s = games_State(i);
    bool live = g.archipelago;
    Heading(live ? "Multiworld" : "Coming soon");
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    if (live)
        ImGui::TextWrapped("Play %s in an Archipelago multiworld: tick the box, fill in the room's server and your slot "
                           "name, then press Play on the Play tab. The game connects as it starts. The room needs this "
                           "game's apworld (below).", g.title);
    else
        ImGui::TextWrapped("Archipelago multiworld randomizer support for %s is coming soon. This page will connect the "
                           "game to a multiworld session when it lands. Until then these fields are saved but not used.",
                           g.title);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8 * sc));
    float lw = 120 * sc, fw = std::min(ImGui::GetContentRegionAvail().x - lw, 380 * sc);
    bool changed = false;
    char buf[256];
    ImGui::BeginDisabled(!live);
    if (ImGui::Checkbox("Play this game in an Archipelago multiworld", &s.ap_enabled)) changed = true;
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4 * sc));
    auto field = [&](const char* label, std::string& v, ImGuiInputTextFlags flags) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(fw);
        snprintf(buf, sizeof buf, "%s", v.c_str());
        ImGui::PushID(label);
        if (ImGui::InputText("##f", buf, sizeof buf, flags)) { v = buf; changed = true; }
        ImGui::PopID();
    };
    field("Server", s.ap_server, 0);
    field("Slot name", s.ap_slot, 0);
    field("Password", s.ap_password, ImGuiInputTextFlags_Password);
    if (changed) games_Save();
    ImGui::Dummy(ImVec2(0, 8 * sc));
    char url[200];
    if (live) {
        ImGui::TextDisabled(s.ap_enabled && !s.ap_slot.empty() ? "Play connects to %s as %s."
                                                                : "Off: Play starts an ordinary game.",
                            s.ap_server.c_str(), s.ap_slot.c_str());
        ImGui::Dummy(ImVec2(0, 12 * sc));
        Rule(sc);
        Heading("The apworld");
        snprintf(url, sizeof url, "https://github.com/%s/releases/latest", g.project);
        ImGui::TextWrapped("Whoever hosts the room adds %s.apworld to Archipelago; it is attached to the game's newest "
                           "release.", g.id[2] == '1' ? "hpps" : g.id);
        Link(url, url);
    } else {
        RuleButton("Connect", ImVec2(140 * sc, 0), false);
        ImGui::SameLine();
        ImGui::TextDisabled("Coming with the Archipelago update");
        ImGui::Dummy(ImVec2(0, 12 * sc));
        Rule(sc);
        Heading("Where to follow it");
        snprintf(url, sizeof url, "https://github.com/%s", g.project);
        Link(url, url);
    }
    Link("archipelago.gg", "https://archipelago.gg");
}

void DrawGamePage(int i, ImVec2 a, ImVec2 b, float sc) {
    const GameInfo& g = games_Info(i);
    ImU32 accent = Binding(g.hue);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // a binding-coloured ribbon down the page's left edge, like a bookmark
    dl->AddRectFilled(ImVec2(a.x + 30 * sc, a.y), ImVec2(a.x + 38 * sc, a.y + 70 * sc), accent);
    dl->AddTriangleFilled(ImVec2(a.x + 30 * sc, a.y + 70 * sc), ImVec2(a.x + 38 * sc, a.y + 70 * sc),
                          ImVec2(a.x + 34 * sc, a.y + 64 * sc), kPaper);

    ImGui::SetCursorScreenPos(ImVec2(a.x + 60 * sc, a.y + 30 * sc));
    ImGui::BeginChild("page", ImVec2(b.x - a.x - 100 * sc, b.y - a.y - 50 * sc), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(g_small);
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    ImGui::Text("BOOK %s  \xC2\xB7  XBOX, %s", kNumerals[i], g.year);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_title);
    ImGui::TextUnformatted(g.title);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    if (g.alt_title) ImGui::Text("Harry Potter and the %s  \xC2\xB7  also known as %s", g.title, g.alt_title);
    else ImGui::Text("Harry Potter and the %s", g.title);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, V(g.playable ? kGood : kWarn));
    ImGui::TextUnformatted(g.stage);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10 * sc));

    // tabs as words with a rule under the chosen one
    const char* tabs[] = {"Play", g.archipelago ? "Archipelago" : "Archipelago (coming soon)"};
    for (int t = 0; t < 2; t++) {
        if (t) ImGui::SameLine(0, 26 * sc);
        ImGui::PushStyleColor(ImGuiCol_Text, V(g_tab == t ? kInk : kInkFaint));
        ImGui::TextUnformatted(tabs[t]);
        ImGui::PopStyleColor();
        ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) g_tab = t;
        if (g_tab == t) dl->AddRectFilled(ImVec2(ra.x, rb.y + 3 * sc), ImVec2(rb.x, rb.y + 5 * sc), accent);
    }
    Rule(sc);
    ImGui::Dummy(ImVec2(0, 4 * sc));
    if (g_tab == 0) DrawPlayTab(i, sc);
    else DrawArchipelagoTab(i, sc);
    ImGui::EndChild();
}

void DrawColophon(ImVec2 a, ImVec2 b, float sc) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + 60 * sc, a.y + 30 * sc));
    ImGui::BeginChild("colophon", ImVec2(b.x - a.x - 100 * sc, b.y - a.y - 50 * sc), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(g_small);
    ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
    ImGui::TextUnformatted("COLOPHON  \xC2\xB7  BARTHES LAUNCHER " BARTHES_VERSION);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_title);
    ImGui::TextUnformatted("With thanks");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * sc));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "MiddleDucky made all of this possible. Their Xbox ports of Philosopher's Stone and Chamber of Secrets are the "
        "foundation every game on this shelf stands on: the recompiler work, the renderer, the audio, the input, the "
        "launchers whose disc unpacking and file checks this launcher reuses, and the fixes Prisoner of Azkaban is being "
        "built from. Thank you, MiddleDucky, truly and enormously.");
    ImGui::PopTextWrapPos();
    Link("github.com/MiddleDucky", "https://github.com/MiddleDucky");
    ImGui::Dummy(ImVec2(0, 8 * sc));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Thanks to Athelas, whose launcher showed what a good home for a recomp can be.");
    ImGui::TextUnformatted("Built with Dear ImGui (Omar Cornut, MIT). Disc images are read with code from MiddleDucky's "
                           "launchers (MIT).");
    ImGui::TextUnformatted("The games and their data belong to their owners. Bring your own disc: nothing from the "
                           "games is included here.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8 * sc));
    Rule(sc);
    Heading("Launcher");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("When a game starts");
    ImGui::SameLine(200 * sc);
    const char* after[] = {"Minimize the launcher", "Close the launcher", "Keep the launcher open"};
    ImGui::SetNextItemWidth(260 * sc);
    if (ImGui::Combo("##after", &games_AfterStart(), after, 3)) games_Save();
    if (RuleButton("Check for game updates")) {
        games_CheckReleasesAsync();
        Say("Checking GitHub...", false);
    }
    ImGui::SameLine();
    if (RuleButton("Open the launcher's folder")) ShellExecuteW(nullptr, L"open", games_ExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Dummy(ImVec2(0, 8 * sc));
    Link("github.com/Barthes-Recompiled", "https://github.com/Barthes-Recompiled");
    ImGui::EndChild();
}

void DrawFrame(float sc) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = vp->Pos, b = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    float shelf_w = 22 * sc + (62 * sc + 10 * sc) * games_Count() + 62 * sc * 0.55f + 22 * sc;
    DrawShelf(dl, a, ImVec2(a.x + shelf_w, b.y), sc);
    ImVec2 pa(a.x + shelf_w, a.y), pb = b;
    dl->AddRectFilled(pa, pb, kPaper);
    // the gutter: a soft shadow where the page meets the shelf
    dl->AddRectFilledMultiColor(pa, ImVec2(pa.x + 18 * sc, pb.y), Rgb(0, 0, 0, 60), Rgb(0, 0, 0, 0), Rgb(0, 0, 0, 0), Rgb(0, 0, 0, 60));
    PushPaperStyle();
    if (g_sel < games_Count()) DrawGamePage(g_sel, pa, pb, sc);
    else DrawColophon(pa, pb, sc);
    PopPaperStyle();
    ImGui::End();
}

void ApplyStyle(float sc) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.FrameRounding = 3;
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.WindowRounding = 0;
    st.ScrollbarSize = 10;
    st.ScrollbarRounding = 5;
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = V(Rgb(38, 33, 30, 60));
    st.ScaleAllSizes(sc);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // one launcher at a time: a second start brings the first forward
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\barthes_launcher");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"BarthesLauncher", nullptr)) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BarthesLauncher";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    float sc = dpi / 96.0f;
    RECT r{0, 0, LONG(1060 * sc), LONG(700 * sc)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Barthes Launcher", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr,
                             nullptr, inst, nullptr);
    BOOL dark = TRUE;  // the title bar matches the shelf
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (!InitD3D(g_hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 could not be started.", L"Barthes Launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    sc = GetDpiForWindow(g_hwnd) / 96.0f;
    ApplyStyle(sc);
    LoadFonts(sc);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    games_Load();
    games_RefreshAll();
    games_CheckReleasesAsync();
    // open on the first game that is installed, else the first; --page <hp1|hp2|hp3|colophon> [--tab archipelago]
    // picks one (for screenshots and tests)
    for (int i = 0; i < games_Count(); i++)
        if (games_State(i).installed) { g_sel = i; break; }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int k = 1; argv && k + 1 < argc; k++) {
        std::string v = Narrow(argv[k + 1]);
        if (!wcscmp(argv[k], L"--page")) {
            if (v == "colophon") g_sel = games_Count();
            for (int i = 0; i < games_Count(); i++)
                if (v == games_Info(i).id) g_sel = i;
        } else if (!wcscmp(argv[k], L"--tab")) {
            g_tab = v == "archipelago" ? 1 : 0;
        } else if (!wcscmp(argv[k], L"--play")) {
            std::string err;
            if (!games_Play(g_sel, false, err)) Say(err, true);
        } else if (!wcscmp(argv[k], L"--install")) {
            g_install_when_known = true;
        } else if (!wcscmp(argv[k], L"--verify") || !wcscmp(argv[k], L"--import")) {
            // unattended tests: verify the page's game, or bring in a disc for it
            std::string err;
            bool ok = argv[k][2] == L'v' ? games_StartVerify(g_sel) : games_StartImport(g_sel, argv[k + 1], err);
            if (!ok) Say(err.empty() ? "Could not start." : err, true);
        }
    }
    if (argv) LocalFree(argv);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_minimized) {
            games_Poll();
            Sleep(100);
            continue;
        }
        float now = GetDpiForWindow(g_hwnd) / 96.0f;
        if (std::fabs(now - sc) > 0.01f) {  // moved to a monitor with another scale
            sc = now;
            ApplyStyle(sc);
            LoadFonts(sc);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
        if (g_refresh) {
            g_refresh = false;
            if (!games_Job().running) games_RefreshAll();
        }
        games_Poll();
        if (g_install_when_known && g_sel < games_Count() && games_State(g_sel).release_checked) {
            g_install_when_known = false;
            if (!games_StartInstall(g_sel)) Say("No release to install.", true);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawFrame(sc);
        ImGui::Render();
        const float clear[4] = {0, 0, 0, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    if (games_Job().running) {
        games_CancelJob();
        games_FinishJob();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(g_hwnd);
    return 0;
}
