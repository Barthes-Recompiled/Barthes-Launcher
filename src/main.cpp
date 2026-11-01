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
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "games.h"
#include "pad.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef BARTHES_VERSION
#define BARTHES_VERSION "1.0.2"
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
        pad_Hold();  // coming or going, what's held now belongs to the other window
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
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, V(kInk));  // the controller's focus: an ink frame
    ImGui::PushStyleColor(ImGuiCol_Button, V(Rgb(226, 216, 196)));  // a dropdown's arrow
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(Rgb(232, 223, 205)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V(Rgb(220, 208, 186)));
}
void PopPaperStyle() { ImGui::PopStyleColor(17); }

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

// ── where the controller is ──────────────────────────────────────────────────────────────────────────────────────
// Two places: the shelf, where left and right pull out the books (ImGui's navigation sleeps), and the page, where
// ImGui's navigation moves between the controls. A goes from the shelf into the page, B back out.
enum Zone { ZONE_SHELF, ZONE_PAGE };
int g_zone = ZONE_PAGE;
bool g_focus_page = true;      // the page's first usable control (Play, if it can) takes the focus this frame
bool g_nav_shown = false;      // the controller or the keys were used last, not the mouse: focus marks show
bool g_ui_holds_back = false;  // last frame a dropdown, the quit question or a text field had B and Esc for itself
bool g_quit_ask = false;

// ── couch mode: full screen, for a controller (Quiver, Steam Big Picture, a TV) ─────────────────────────────────
// Play minimizes the launcher and waits for the game, then brings it back, since whatever started the launcher
// (Quiver) is watching its process: closing on start would tell it the session ended.
bool g_couch = false;
bool g_waiting = false;          // a game started from couch mode is running; the launcher waits, minimized
bool g_quit_after_game = false;  // --quit-after-game: once that game ends, so does the launcher
RECT g_window_rect{};            // the window's place before it went full screen

void SetCouch(bool on) {
    if (on == g_couch) return;
    g_couch = on;
    if (on) {
        GetWindowRect(g_hwnd, &g_window_rect);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        const RECT& r = g_window_rect;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

// Steam's Big Picture (and a Steam Deck's Game Mode) starts the launcher in couch mode
bool StartedByBigPicture() {
    wchar_t v[8];
    if (GetEnvironmentVariableW(L"SteamTenfoot", v, 8) && v[0] == L'1') return true;
    if (GetEnvironmentVariableW(L"SteamDeck", v, 8) && v[0] == L'1') return true;
    return GetEnvironmentVariableW(L"SteamGamepadUI", v, 8) > 0;
}

// after the game: the launcher back in front, the focus on Play again
void ComeBack() {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), them = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = them && them != me && AttachThreadInput(me, them, TRUE);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    BringWindowToTop(g_hwnd);
    if (attached) AttachThreadInput(me, them, FALSE);
    if (GetForegroundWindow() != g_hwnd) FlashWindow(g_hwnd, TRUE);  // Windows kept the focus elsewhere
    g_zone = ZONE_PAGE;
    g_focus_page = true;
    pad_Hold();
}

// call after a control: it takes the focus if the page wants it placed and this control can be used. (FocusItem lands
// on that very control; SetKeyboardFocusHere counts controls and can land on a neighbour.)
void FocusThis(bool usable) {
    if (g_focus_page && usable) {
        ImGui::FocusItem();
        g_focus_page = false;
    }
}

void OpenUrl(const char* url) { ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// a link: underlined under the mouse or the controller's focus, opened by a click or A
bool Link(const char* label, const char* url) {
    ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(label);
    bool pressed = ImGui::InvisibleButton(label, ts);
    bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && g_nav_shown;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, Rgb(70, 60, 120), label);
    if (hovered || focused) dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), Rgb(70, 60, 120), focused ? 2.0f : 1.0f);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (pressed) OpenUrl(url);
    return pressed;
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

void SelectBook(int i) {
    g_sel = i;
    g_tab = 0;
    g_message.clear();
    if (i < games_Count()) games_Refresh(i);
}

bool CanPlay(int i) {
    const GameState& s = games_State(i);
    return i < games_Count() && !games_Job().running && s.installed && !s.running && !s.wrong_game && s.has_disc;
}

bool CanOpenSettings(int i) {
    const GameState& s = games_State(i);
    return i < games_Count() && !games_Job().running && s.installed && !s.running;
}

// Play (settings=false) or Game settings
void StartGame(int i, bool settings) {
    std::string err;
    if (!games_Play(i, settings, err)) {
        Say(err, true);
        return;
    }
    if (g_couch || g_quit_after_game) {  // wait for the game, then come back (or quit) after it
        Say("", false);
        g_waiting = true;
        ShowWindow(g_hwnd, SW_MINIMIZE);
        return;
    }
    if (settings) return;
    Say("", false);
    if (games_AfterStart() == AFTER_CLOSE) PostQuitMessage(0);
    else if (games_AfterStart() == AFTER_MINIMIZE) ShowWindow(g_hwnd, SW_MINIMIZE);
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
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // the controller moves along the shelf by itself (HandleActions)
        if (ImGui::InvisibleButton("book", ImVec2(w, h))) {
            SelectBook(i);
            g_zone = ZONE_SHELF;
        }
        ImGui::PopItemFlag();
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImU32 cloth = colophon ? Rgb(196, 186, 164) : Binding(games_Info(i).hue, hov ? 0.52f : 0.46f);
        ImU32 band = colophon ? Rgb(150, 140, 120) : Binding(games_Info(i).hue, 0.30f, 0.45f);
        ImU32 letter = colophon ? Rgb(60, 52, 44) : Rgb(236, 222, 190);
        dl->AddRectFilled(ImVec2(p0.x + 3 * sc, p0.y + 3 * sc), ImVec2(p1.x + 3 * sc, p1.y), Rgb(0, 0, 0, 70), 3 * sc);
        dl->AddRectFilled(p0, p1, cloth, 3 * sc);
        // the controller on the shelf: a thin gilt line round the book pulled out
        if (g_zone == ZONE_SHELF && g_nav_shown && g_sel == i)
            dl->AddRect(ImVec2(p0.x - 4 * sc, p0.y - 4 * sc), ImVec2(p1.x + 4 * sc, p1.y + 4 * sc), Rgb(214, 190, 130), 5 * sc, 0, 2 * sc);
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

// ── choosing without a mouse: a file browser and an on-screen keyboard ─────────────────────────────────────────
// The Windows file dialog and text fields need a mouse or keyboard, so with the controller in hand (or in couch mode)
// the page asks here instead: a browser of the computer's places and folders, and a keyboard of keys to press.

bool PadMode() { return g_couch || (pad_Connected() && g_nav_shown); }

void UseFolder(int i, const std::wstring& dir) {
    GameState& s = games_State(i);
    s.folder = dir;
    s.custom_folder = true;
    games_Save();
    games_Refresh(i);
    Say(s.installed ? "Found the game there." : "No build there yet: installing will put it in that folder.", false);
}

void UseDisc(int i, const std::wstring& src) {
    std::string err;
    if (!games_StartImport(i, src, err)) Say(err, true);
    else Say("", false);
}

struct BrowseEntry {
    std::wstring path;
    std::string label;
    bool dir;
};

struct Browser {
    bool want_open = false, shown = false;
    int game = 0;
    bool folder = false;          // choosing a folder (Use another folder), else a disc image or default.xbe
    std::wstring dir;             // empty: the places to start from
    std::vector<BrowseEntry> entries;
    std::wstring focus;           // the entry to put the focus on once listed (the folder just left); empty: the first
    bool refocus = false;
    std::wstring last;            // where the last choice was made, offered first next time
} g_browse;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

void BrowseTo(const std::wstring& dir, const std::wstring& focus = L"") {
    Browser& b = g_browse;
    b.dir = dir;
    b.entries.clear();
    b.focus = focus;
    b.refocus = true;
    if (dir.empty()) {
        auto place = [&](const std::wstring& path, const char* name) {
            if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                b.entries.push_back({path, std::string(name) + "   " + Narrow(path), true});
        };
        place(b.last, "Last used");
        place(KnownFolder(FOLDERID_Downloads), "Downloads");
        place(KnownFolder(FOLDERID_Desktop), "Desktop");
        place(KnownFolder(FOLDERID_Documents), "Documents");
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(drives & (1u << d))) continue;
            wchar_t root[] = {wchar_t(L'A' + d), L':', L'\\', 0};
            UINT type = GetDriveTypeW(root);
            if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
            const char* kind = type == DRIVE_REMOVABLE ? "Removable drive" : type == DRIVE_CDROM ? "Disc drive"
                             : type == DRIVE_REMOTE ? "Network drive" : "Drive";
            b.entries.push_back({std::wstring(root, 2), std::string(kind) + "   " + Narrow(std::wstring(root, 2)), true});
        }
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        bool is_dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        if (!is_dir) {
            if (b.folder) continue;
            std::wstring low = Lower(name);
            auto ends = [&](const wchar_t* ext) {
                size_t n = wcslen(ext);
                return low.size() >= n && low.compare(low.size() - n, n, ext) == 0;
            };
            if (!ends(L".iso") && !ends(L".xiso") && low != L"default.xbe") continue;
        }
        b.entries.push_back({dir + L"\\" + name, Narrow(name) + (is_dir ? "\\" : ""), is_dir});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(b.entries.begin(), b.entries.end(), [](const BrowseEntry& x, const BrowseEntry& y) {
        if (x.dir != y.dir) return x.dir;
        return Lower(x.path) < Lower(y.path);
    });
}

void OpenBrowser(int game, bool folder) {
    g_browse.game = game;
    g_browse.folder = folder;
    g_browse.want_open = true;
    BrowseTo(L"");
}

std::wstring UpFrom(const std::wstring& dir) {
    size_t cut = dir.find_last_of(L'\\');
    return cut == std::wstring::npos ? L"" : dir.substr(0, cut);  // "C:" goes up to the places
}

void DrawBrowser(float sc) {
    Browser& b = g_browse;
    const char* id = "##browser";
    if (b.want_open) {
        ImGui::OpenPopup(id);
        b.want_open = false;
    }
    b.shown = false;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - 40 * sc, 760 * sc), vp->Size.y * 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 22 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f * sc);
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        b.shown = true;
        bool appearing = ImGui::IsWindowAppearing();
        const char* title = games_Info(b.game).title;
        ImGui::PushFont(g_spine);
        ImGui::Text(b.folder ? "A folder for %s" : "Your %s disc", title);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, V(kInkSoft));
        ImGui::TextWrapped("%s", b.dir.empty() ? (b.folder ? "Places on this computer. Open one, then choose the folder."
                                                           : "Places on this computer. Open one to find your .iso, or default.xbe in an extracted game folder.")
                                               : Narrow(b.dir).c_str());
        ImGui::PopStyleColor();
        Rule(sc);
        int act = -1;
        bool up = false;
        float foot = ImGui::GetFrameHeightWithSpacing() + 12 * sc;
        ImGui::BeginChild("##list", ImVec2(0, -foot), ImGuiChildFlags_NavFlattened);
        if (!b.dir.empty()) {
            if (ImGui::Selectable("..   Up a level")) up = true;
            if (b.refocus && b.focus.empty()) ImGui::FocusItem();
        }
        if (b.entries.empty() && !b.dir.empty()) ImGui::TextDisabled(b.folder ? "No folders here." : "Nothing here the launcher can read.");
        for (int k = 0; k < (int)b.entries.size(); k++) {
            const BrowseEntry& e = b.entries[k];
            ImGui::PushID(k);
            if (ImGui::Selectable(e.label.c_str())) act = k;
            ImGui::PopID();
            if (b.refocus && ((b.focus.empty() && b.dir.empty() && k == 0) || (!b.focus.empty() && e.path == b.focus)))
                ImGui::FocusItem();
        }
        b.refocus = false;
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 4 * sc));
        bool cancel = RuleButton("Cancel", ImVec2(140 * sc, 0));
        bool choose_here = false;
        if (b.folder && !b.dir.empty()) {
            ImGui::SameLine(0, 12 * sc);
            choose_here = InkButton("Choose this folder", ImVec2(0, 0), Binding(games_Info(b.game).hue));
        }
        unsigned pr = pad_Pressed();
        if (!appearing && ((pr & PAD_B) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
            if (b.dir.empty()) cancel = true;
            else up = true;
        }
        if ((pr & PAD_Y) && b.folder && !b.dir.empty()) choose_here = true;

        if (cancel) {
            ImGui::CloseCurrentPopup();
        } else if (choose_here) {
            b.last = b.dir;
            ImGui::CloseCurrentPopup();
            UseFolder(b.game, b.dir);
        } else if (up) {
            std::wstring from = b.dir;
            BrowseTo(UpFrom(b.dir), from);
        } else if (act >= 0) {
            BrowseEntry e = b.entries[act];
            if (e.dir) {
                BrowseTo(e.path);
            } else {
                b.last = b.dir;
                ImGui::CloseCurrentPopup();
                UseDisc(b.game, e.path);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

struct Keyboard {
    bool want_open = false, shown = false;
    std::string* target = nullptr;
    std::string label, text;
    bool password = false, shift = false;
} g_keys;

void OpenKeyboard(std::string& target, const char* label, bool password) {
    g_keys.target = &target;
    g_keys.label = label;
    g_keys.text = target;
    g_keys.password = password;
    g_keys.shift = false;
    g_keys.want_open = true;
}

void DrawKeyboard(float sc) {
    Keyboard& k = g_keys;
    const char* id = "##keyboard";
    ImGuiIO& io = ImGui::GetIO();
    if (k.want_open) {
        ImGui::OpenPopup(id);
        k.want_open = false;
    }
    k.shown = false;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 22 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * sc, 6 * sc));
    bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
    // a real keyboard types straight in while this is up, so its Space and Enter mustn't also press the focused key
    if (open) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (open) {
        k.shown = true;
        bool appearing = ImGui::IsWindowAppearing();
        ImGui::PushFont(g_spine);
        ImGui::TextUnformatted(k.label.c_str());
        ImGui::PopFont();
        // the text so far, in a field-like box, with a caret
        float kw = 46 * sc, kh = 42 * sc, row_w = kw * 10 + 6 * sc * 9;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float fh = ImGui::GetFrameHeight() + 6 * sc;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + row_w, p.y + fh), Rgb(250, 246, 237), 3 * sc);
        dl->AddRect(p, ImVec2(p.x + row_w, p.y + fh), kPaperEdge, 3 * sc);
        std::string shown = k.password ? std::string(k.text.size(), '*') : k.text;
        if (fmod(ImGui::GetTime(), 1.0) < 0.6) shown += "|";
        ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
        float tx = std::min(0.0f, row_w - 24 * sc - ts.x);  // the end stays in view as it grows
        dl->PushClipRect(p, ImVec2(p.x + row_w, p.y + fh), true);
        dl->AddText(ImVec2(p.x + 12 * sc + tx, p.y + (fh - ts.y) * 0.5f), kInk, shown.c_str());
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(row_w, fh + 6 * sc));

        static const char* const rows[2][4] = {
            {"1234567890", "qwertyuiop", "asdfghjkl:", "zxcvbnm.-_"},
            {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL;", "ZXCVBNM,=/"},
        };
        std::string typed;
        bool del = false, done = false, cancel = false;
        for (int r = 0; r < 4; r++) {
            const char* keys = rows[k.shift][r];
            for (int c = 0; keys[c]; c++) {
                if (c) ImGui::SameLine();
                char cap[2] = {keys[c], 0};
                ImGui::PushID(r * 16 + c);
                if (ImGui::Button(cap, ImVec2(kw, kh))) typed += keys[c];
                if (appearing && r == 1 && c == 0) ImGui::FocusItem();
                ImGui::PopID();
            }
        }
        float wide = (row_w - 6 * sc * 4) / 5;
        if (RuleButton(k.shift ? "abc" : "Shift", ImVec2(wide, kh))) k.shift = !k.shift;
        ImGui::SameLine();
        if (RuleButton("Space", ImVec2(wide, kh))) typed += ' ';
        ImGui::SameLine();
        if (RuleButton("Delete", ImVec2(wide, kh))) del = true;
        ImGui::SameLine();
        if (RuleButton("Paste", ImVec2(wide, kh)))
            if (const char* clip = ImGui::GetClipboardText())
                for (const char* c = clip; *c; c++)
                    if ((unsigned char)*c >= 32) typed += *c;
        ImGui::SameLine();
        done = InkButton("Done", ImVec2(wide, kh), kInk);

        // the controller's shortcuts, and a real keyboard
        unsigned pr = pad_Pressed();
        if (pr & PAD_X) del = true;
        if (pr & PAD_Y) k.shift = !k.shift;
        if (pr & PAD_START) done = true;
        if (!appearing) {
            if (pr & PAD_B) cancel = true;
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 32 && c < 127) typed += char(c);
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) del = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) done = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
            if (ImGui::IsKeyPressed(ImGuiKey_V, false) && io.KeyCtrl)
                if (const char* clip = ImGui::GetClipboardText())
                    for (const char* c = clip; *c; c++)
                        if ((unsigned char)*c >= 32) typed += *c;
        }
        if (del && !k.text.empty()) k.text.pop_back();
        k.text += typed;
        if (k.text.size() > 250) k.text.resize(250);
        if (done && k.target) {
            *k.target = k.text;
            games_Save();
        }
        if (done || cancel) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
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
    FocusThis(true);
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
    } else {
        if (InkButton("Play", ImVec2(bw, bh), accent, can_play)) StartGame(i, false);
        FocusThis(can_play);
    }
    ImGui::SameLine(0, 12 * sc);
    if (RuleButton("Game settings", ImVec2(160 * sc, bh), s.installed && !s.running)) StartGame(i, true);
    FocusThis(s.installed && !s.running);
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
        FocusThis(!s.running);
        ImGui::SameLine();
    }
    if (RuleButton("Use another folder...")) {
        std::wstring dir;
        if (PadMode()) OpenBrowser(i, true);
        else if (PickFile(dir, true, L"Choose the folder with the game's exe (or an empty folder to install into)"))
            UseFolder(i, dir);
    }
    FocusThis(true);
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
        if (PadMode()) OpenBrowser(i, false);
        else if (PickFile(src, false, L"Choose your disc image (.iso), or default.xbe in an extracted game folder"))
            UseDisc(i, src);
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
    FocusThis(live);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4 * sc));
    auto field = [&](const char* label, std::string& v, ImGuiInputTextFlags flags) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(fw);
        ImGui::PushID(label);
        if (PadMode()) {  // no keyboard to hand: the field is a button that opens the on-screen keyboard
            bool password = flags & ImGuiInputTextFlags_Password;
            std::string shown = v.empty() ? "(empty)" : password ? std::string(v.size(), '*') : v;
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleColor(ImGuiCol_Text, V(v.empty() ? kInkFaint : kInk));
            ImGui::BeginDisabled(!live);
            if (ImGui::Button((shown + "##f").c_str(), ImVec2(fw, 0))) OpenKeyboard(v, label, password);
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        } else {
            snprintf(buf, sizeof buf, "%s", v.c_str());
            if (ImGui::InputText("##f", buf, sizeof buf, flags)) { v = buf; changed = true; }
        }
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
    FocusThis(true);  // nothing above to start on
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
    ImGui::BeginChild("page", ImVec2(b.x - a.x - 100 * sc, b.y - a.y - 50 * sc), ImGuiChildFlags_NavFlattened,
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
        for (ImGuiCol c : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
            ImGui::PushStyleColor(c, ImVec4(0, 0, 0, 0));
        ImGui::PushID(t);
        if (ImGui::Selectable(tabs[t], false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(tabs[t]))) g_tab = t;
        ImGui::PopID();
        ImGui::PopStyleColor(4);
        ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    ImGui::BeginChild("colophon", ImVec2(b.x - a.x - 100 * sc, b.y - a.y - 50 * sc), ImGuiChildFlags_NavFlattened,
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
    FocusThis(true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Controller buttons");
    ImGui::SameLine(200 * sc);
    const char* labels[] = {"Xbox", "PlayStation", "Nintendo"};
    ImGui::SetNextItemWidth(260 * sc);
    if (ImGui::Combo("##labels", &games_PadLabels(), labels, 3)) games_Save();
    if (ImGui::Checkbox("Show what the controller's buttons do along the bottom", &games_PadHints())) games_Save();
    if (ImGui::Checkbox("Couch mode: start full screen, for a controller (View or F11 switches)", &games_Couch())) {
        SetCouch(games_Couch());
        games_Save();
    }
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

// ── the controller's hints, along the foot of the page ─────────────────────────────────────────────────────────

enum Glyph { G_A, G_B, G_X, G_Y, G_SHOULDERS, G_START, G_VIEW, G_LEFT_RIGHT };

bool HintsShown() { return pad_Connected() && games_PadHints(); }

// one button as the chosen controller labels it: face buttons as ink discs, the rest as ink pills
float DrawGlyph(ImDrawList* dl, Glyph g, ImVec2 p, float h, float sc) {
    int kind = games_PadLabels();
    ImFont* f = g_small;
    auto pill = [&](const char* text) {
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, text);
        float w = std::max(h, ts.x + h * 0.7f);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kInk, h * 0.5f);
        dl->AddText(f, f->FontSize, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), kPaper, text);
        return w;
    };
    if (g == G_SHOULDERS) {
        const char* l = kind == LABELS_PLAYSTATION ? "L1" : kind == LABELS_NINTENDO ? "L" : "LB";
        const char* r = kind == LABELS_PLAYSTATION ? "R1" : kind == LABELS_NINTENDO ? "R" : "RB";
        float w = pill(l) + 4 * sc;
        p.x += w;
        return w + pill(r);
    }
    if (g == G_START) return pill(kind == LABELS_PLAYSTATION ? "Options" : kind == LABELS_NINTENDO ? "+" : "Menu");
    if (g == G_VIEW) return pill(kind == LABELS_PLAYSTATION ? "Share" : kind == LABELS_NINTENDO ? "-" : "View");
    if (g == G_LEFT_RIGHT) {  // the d-pad's left and right, as two small arrows
        float w = h * 1.5f, r = h * 0.22f;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kInk, h * 0.5f);
        ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x - w * 0.36f, c.y), ImVec2(c.x - w * 0.36f + r * 1.3f, c.y - r), ImVec2(c.x - w * 0.36f + r * 1.3f, c.y + r), kPaper);
        dl->AddTriangleFilled(ImVec2(c.x + w * 0.36f, c.y), ImVec2(c.x + w * 0.36f - r * 1.3f, c.y + r), ImVec2(c.x + w * 0.36f - r * 1.3f, c.y - r), kPaper);
        return w;
    }
    ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(c, h * 0.5f, kInk);
    if (kind == LABELS_PLAYSTATION) {  // drawn, not from a font: cross, circle, square, triangle
        float r = h * 0.22f, t = 1.6f * sc;
        if (g == G_A) {
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), kPaper, t);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), kPaper, t);
        } else if (g == G_B) {
            dl->AddCircle(c, r * 1.1f, kPaper, 0, t);
        } else if (g == G_X) {
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), kPaper, 0, 0, t);
        } else {
            dl->AddTriangle(ImVec2(c.x, c.y - r * 1.2f), ImVec2(c.x + r * 1.2f, c.y + r * 0.9f), ImVec2(c.x - r * 1.2f, c.y + r * 0.9f), kPaper, t);
        }
        return h;
    }
    // by place: Nintendo's bottom button says B, its right one A, its left Y, its top X
    static const char* const xbox[] = {"A", "B", "X", "Y"};
    static const char* const nintendo[] = {"B", "A", "Y", "X"};
    const char* text = (kind == LABELS_NINTENDO ? nintendo : xbox)[g];
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, text);
    dl->AddText(f, f->FontSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kPaper, text);
    return h;
}

void DrawHints(ImDrawList* dl, ImVec2 a, ImVec2 b, float sc) {
    struct Hint { Glyph g; const char* does; };
    Hint hints[7];
    int n = 0;
    bool game = g_sel < games_Count();
    if (g_keys.shown) {
        hints[n++] = {G_A, "Type"};
        hints[n++] = {G_X, "Delete"};
        hints[n++] = {G_Y, "Shift"};
        hints[n++] = {G_START, "Done"};
        hints[n++] = {G_B, "Cancel"};
    } else if (g_browse.shown) {
        hints[n++] = {G_A, "Open"};
        hints[n++] = {G_B, g_browse.dir.empty() ? "Cancel" : "Up a level"};
        if (g_browse.folder && !g_browse.dir.empty()) hints[n++] = {G_Y, "Choose this folder"};
    } else if (g_ui_holds_back) {
        hints[n++] = {G_A, "Select"};
        hints[n++] = {G_B, "Back"};
    } else if (g_zone == ZONE_SHELF) {
        hints[n++] = {G_LEFT_RIGHT, "Choose a book"};
        hints[n++] = {G_A, "Open"};
        hints[n++] = {G_B, "Quit"};
        if (CanPlay(g_sel)) hints[n++] = {G_START, "Play"};
    } else {
        hints[n++] = {G_A, "Select"};
        hints[n++] = {G_B, "Shelf"};
        if (game) hints[n++] = {G_SHOULDERS, "Play / Archipelago"};
        if (CanPlay(g_sel)) hints[n++] = {G_START, "Play"};
        if (CanOpenSettings(g_sel)) hints[n++] = {G_Y, "Game settings"};
        hints[n++] = {G_VIEW, g_couch ? "Window" : "Full screen"};
    }
    dl->AddLine(ImVec2(a.x + 60 * sc, a.y), ImVec2(b.x - 40 * sc, a.y), kPaperEdge, 1.0f * sc);
    float h = 24 * sc, x = a.x + 60 * sc, y = (a.y + b.y - h) * 0.5f;
    for (int k = 0; k < n; k++) {
        x += DrawGlyph(dl, hints[k].g, ImVec2(x, y), h, sc) + 8 * sc;
        ImVec2 ts = g_small->CalcTextSizeA(g_small->FontSize, FLT_MAX, 0, hints[k].does);
        dl->AddText(g_small, g_small->FontSize, ImVec2(x, y + (h - ts.y) * 0.5f), kInkSoft, hints[k].does);
        x += ts.x + 24 * sc;
    }
}

// B on the shelf: a small page over the launcher, Quit already chosen so B then A goes back to Quiver
void DrawQuitQuestion(float sc) {
    const char* name = "Quit Barthes Launcher?";
    if (g_quit_ask) {
        ImGui::OpenPopup(name);
        g_quit_ask = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * sc, 26 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f * sc);
    if (ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g_spine);
        ImGui::TextUnformatted(name);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10 * sc));
        bool appearing = ImGui::IsWindowAppearing();  // opened by this frame's B or Esc, which mustn't close it again
        if (InkButton("Quit", ImVec2(140 * sc, 40 * sc), kInk)) PostQuitMessage(0);
        if (appearing) ImGui::FocusItem();
        ImGui::SameLine(0, 12 * sc);
        bool stay = RuleButton("Stay", ImVec2(140 * sc, 40 * sc));
        if (stay || (!appearing && ((pad_Pressed() & PAD_B) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// What the controller's buttons (and the matching keys) do outside ImGui's own navigation: moving along the shelf,
// going into the page and back out, the tabs, Play and Game settings.
void HandleActions() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned pr = pad_Pressed();
    bool typing = io.WantTextInput;
    auto key = [&](ImGuiKey k) { return !typing && ImGui::IsKeyPressed(k, false); };
    bool kb_left = key(ImGuiKey_LeftArrow) || key(ImGuiKey_UpArrow), kb_right = key(ImGuiKey_RightArrow) || key(ImGuiKey_DownArrow);
    bool kb_enter = key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter), kb_esc = key(ImGuiKey_Escape);
    if (pr || kb_left || kb_right || kb_enter || kb_esc || key(ImGuiKey_Tab)) {
        g_nav_shown = true;
    } else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(0)) {
        g_nav_shown = false;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;  // the quit question or a dropdown has the buttons
    if ((pr & PAD_BACK) || key(ImGuiKey_F11)) SetCouch(!g_couch);

    bool back = (pr & PAD_B) || kb_esc;
    if (g_zone == ZONE_SHELF) {
        int last = games_Count();  // the colophon
        if (((pr & (PAD_LEFT | PAD_UP)) || kb_left) && g_sel > 0) SelectBook(g_sel - 1);
        if (((pr & (PAD_RIGHT | PAD_DOWN)) || kb_right) && g_sel < last) SelectBook(g_sel + 1);
        if ((pr & PAD_A) || kb_enter) {
            g_zone = ZONE_PAGE;
            g_focus_page = true;
        } else if (back) {
            g_quit_ask = true;
        }
    } else if (back && !g_ui_holds_back) {
        g_zone = ZONE_SHELF;
        ImGui::SetWindowFocus(nullptr);  // ImGui's navigation sleeps while the shelf has the controller
    }
    if (g_sel < games_Count()) {
        int tab = (pr & PAD_LB) ? 0 : (pr & PAD_RB) ? 1 : g_tab;
        if (tab != g_tab) {
            g_tab = tab;
            if (g_zone == ZONE_PAGE) g_focus_page = true;
        }
        if ((pr & PAD_START) && CanPlay(g_sel)) StartGame(g_sel, false);
        else if ((pr & PAD_Y) && CanOpenSettings(g_sel)) StartGame(g_sel, true);
    }
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
    // a click on the page takes the controller there too, as one on a book takes it to the shelf
    if (ImGui::IsMouseClicked(0) && ImGui::GetIO().MousePos.x >= pa.x) g_zone = ZONE_PAGE;
    float foot = HintsShown() ? 46 * sc : 0;
    PushPaperStyle();
    ImVec2 page_end(pb.x, pb.y - foot);
    if (g_sel < games_Count()) DrawGamePage(g_sel, pa, page_end, sc);
    else DrawColophon(pa, page_end, sc);
    g_focus_page = false;  // nothing on the page could take it (a job of another game's running): don't keep asking
    if (foot > 0) DrawHints(dl, ImVec2(pa.x, page_end.y), pb, sc);
    DrawQuitQuestion(sc);
    DrawBrowser(sc);
    DrawKeyboard(sc);
    PopPaperStyle();
    g_ui_holds_back = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::IsAnyItemActive();
    if (g_couch && g_nav_shown) ImGui::SetMouseCursor(ImGuiMouseCursor_None);  // no arrow over a TV while the pad drives
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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    // the controller and the arrow keys move the focus; pad.cpp reads the controller (not the Win32 backend, which
    // would read it while a game is in front too)
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    pad_Init();
    sc = GetDpiForWindow(g_hwnd) / 96.0f;
    ApplyStyle(sc);
    LoadFonts(sc);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    games_Load();
    games_RefreshAll();
    games_CheckReleasesAsync();
    // open on the game played last if it's still installed, else the first that is installed, else the first;
    // --page <hp1|hp2|hp3|colophon> [--tab archipelago] picks one. --couch (or --window) starts in couch mode (or not),
    // whatever the setting; --play starts the page's game at once and --quit-after-game closes the launcher after it,
    // so a Quiver or Steam entry can be one game: --page hp1 --play --couch --quit-after-game
    for (int i = 0; i < games_Count(); i++)
        if (games_State(i).installed) { g_sel = i; break; }
    int last = games_LastPlayed();
    if (last >= 0 && last < games_Count() && games_State(last).installed) g_sel = last;
    bool couch = games_Couch() || StartedByBigPicture(), play = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int k = 1; argv && k < argc; k++) {
        std::string v = k + 1 < argc ? Narrow(argv[k + 1]) : "";
        if (!wcscmp(argv[k], L"--couch")) {
            couch = true;
        } else if (!wcscmp(argv[k], L"--window")) {
            couch = false;
        } else if (!wcscmp(argv[k], L"--quit-after-game")) {
            g_quit_after_game = true;
        } else if (!wcscmp(argv[k], L"--page")) {
            if (v == "colophon") g_sel = games_Count();
            for (int i = 0; i < games_Count(); i++)
                if (v == games_Info(i).id) g_sel = i;
        } else if (!wcscmp(argv[k], L"--tab")) {
            g_tab = v == "archipelago" ? 1 : 0;
        } else if (!wcscmp(argv[k], L"--play")) {
            play = true;  // once the window is up (below)
        } else if (!wcscmp(argv[k], L"--install")) {
            g_install_when_known = true;
        } else if ((!wcscmp(argv[k], L"--verify") || !wcscmp(argv[k], L"--import")) && k + 1 < argc) {
            // unattended tests: verify the page's game, or bring in a disc for it
            std::string err;
            bool ok = argv[k][2] == L'v' ? games_StartVerify(g_sel) : games_StartImport(g_sel, argv[k + 1], err);
            if (!ok) Say(err.empty() ? "Could not start." : err, true);
        }
    }
    if (argv) LocalFree(argv);
    if (couch) SetCouch(true);
    else ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    if (play && g_sel < games_Count()) StartGame(g_sel, false);

    bool done = false, first_frame = true;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_waiting && !games_Running()) {  // the game started from couch mode has ended
            g_waiting = false;
            if (g_quit_after_game) break;
            ComeBack();
            games_RefreshAll();
        }
        if (g_minimized) {
            games_Poll();
            Sleep(100);
            continue;
        }
        float now = GetDpiForWindow(g_hwnd) / 96.0f;
        if (g_couch) {  // as large as the screen allows, the window's 1060x700 layout scaled up to fit
            RECT c;
            GetClientRect(g_hwnd, &c);
            float fit = std::min(c.right / (1060 * now), c.bottom / (700 * now)) * 0.95f;
            if (fit > 1.0f) now *= std::floor(fit * 20.0f) / 20.0f;
        }
        if (std::fabs(now - sc) > 0.01f) {  // moved to a monitor with another scale, or in or out of couch mode
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
        pad_Update(GetForegroundWindow() == g_hwnd);
        pad_Feed(g_zone == ZONE_PAGE || g_ui_holds_back, g_ui_holds_back);
        // ImGui only presses what its focus frame shows, so a controller shows the frame (from the first frame when one
        // is connected, so a single A plays), and the mouse hides it again
        if (pad_Pressed() || (first_frame && pad_Connected())) {
            GImGui->NavDisableHighlight = false;
            GImGui->NavDisableMouseHover = true;
            g_nav_shown = true;
        }
        first_frame = false;
        ImGui::NewFrame();
        HandleActions();
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
