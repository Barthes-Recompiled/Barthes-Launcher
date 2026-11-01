// The controller for the launcher's pages (pad.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include <initializer_list>

#include <imgui.h>

#include "pad.h"

namespace {

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);
GetStateFn g_get_state = nullptr;

bool g_connected[XUSER_MAX_COUNT] = {};
DWORD g_next_probe = 0;   // empty slots are slow to ask, so they're asked once a second
DWORD g_last_update = 0;

// what the controllers hold now, after the gates: the launcher's buttons, and the sticks for ImGui
unsigned g_down = 0, g_prev = 0;
bool g_hold = true;  // held buttons wait for a release (from the start: Quiver's A may still be down)
float g_rs[4] = {};  // the right stick: up, down, left, right, 0..1
unsigned g_fed = 0;  // the buttons handed to ImGui, which went down while handed on

// a stick's direction counts as a d-pad press past half way, and lets go under a third
float Axis(SHORT v, bool positive) {
    float f = positive ? v / 32767.0f : -v / 32768.0f;
    const float dead = 7849.0f / 32767.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    return f <= dead ? 0.0f : (f - dead) / (1.0f - dead);
}

bool Stick(float v, unsigned bit) { return v > ((g_prev & bit) ? 0.33f : 0.5f); }

}  // namespace

void pad_Init() {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(dll)) {
            g_get_state = (GetStateFn)(void*)GetProcAddress(m, "XInputGetState");
            if (g_get_state) break;
        }
}

void pad_Hold() { g_hold = true; }

bool pad_Connected() {
    for (bool c : g_connected)
        if (c) return true;
    return false;
}

unsigned pad_Pressed() { return g_down & ~g_prev; }

void pad_Update(bool live) {
    g_prev = g_down;
    g_down = 0;
    for (float& v : g_rs) v = 0;
    DWORD now = GetTickCount();
    if (now - g_last_update > 250) g_hold = true;  // minimized meanwhile: no frames, so no chance to see a release
    g_last_update = now;
    if (!g_get_state) return;

    bool probe = (LONG)(now - g_next_probe) >= 0;
    if (probe) g_next_probe = now + 1000;
    unsigned raw = 0;
    float ls[4] = {}, rs[4] = {};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        if (!g_connected[i] && !probe) continue;
        XINPUT_STATE st{};
        g_connected[i] = g_get_state(i, &st) == ERROR_SUCCESS;
        if (!g_connected[i]) continue;
        const XINPUT_GAMEPAD& p = st.Gamepad;
        WORD b = p.wButtons;
        const struct { WORD x; unsigned ours; } map[] = {
            {XINPUT_GAMEPAD_DPAD_UP, PAD_UP},   {XINPUT_GAMEPAD_DPAD_DOWN, PAD_DOWN},
            {XINPUT_GAMEPAD_DPAD_LEFT, PAD_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, PAD_RIGHT},
            {XINPUT_GAMEPAD_A, PAD_A},          {XINPUT_GAMEPAD_B, PAD_B},
            {XINPUT_GAMEPAD_X, PAD_X},          {XINPUT_GAMEPAD_Y, PAD_Y},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, PAD_LB}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_RB},
            {XINPUT_GAMEPAD_START, PAD_START},  {XINPUT_GAMEPAD_BACK, PAD_BACK},
        };
        for (const auto& m : map)
            if (b & m.x) raw |= m.ours;
        // every connected pad counts, so whichever one is picked up works
        float l[4] = {Axis(p.sThumbLY, true), Axis(p.sThumbLY, false), Axis(p.sThumbLX, false), Axis(p.sThumbLX, true)};
        float r[4] = {Axis(p.sThumbRY, true), Axis(p.sThumbRY, false), Axis(p.sThumbRX, false), Axis(p.sThumbRX, true)};
        for (int k = 0; k < 4; k++) {
            if (l[k] > ls[k]) ls[k] = l[k];
            if (r[k] > rs[k]) rs[k] = r[k];
        }
    }
    const unsigned dirs[4] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
    for (int k = 0; k < 4; k++)
        if (Stick(ls[k], dirs[k])) raw |= dirs[k];
    bool any = raw || rs[0] > 0.33f || rs[1] > 0.33f || rs[2] > 0.33f || rs[3] > 0.33f;

    if (!live) {  // a game (or anything else) is in front: its presses aren't ours
        g_hold = true;
        g_prev = 0;
        return;
    }
    if (g_hold) {
        if (any) {
            g_prev = 0;
            return;
        }
        g_hold = false;
    }
    g_down = raw;
    for (int k = 0; k < 4; k++) g_rs[k] = rs[k];
}

void pad_Feed(bool nav, bool cancel) {
    ImGuiIO& io = ImGui::GetIO();
    if (pad_Connected()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

    unsigned allowed = nav ? (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A) : 0;
    if (cancel) allowed |= PAD_B;
    unsigned pressed = g_down & ~g_prev;
    g_fed = (g_fed & g_down & allowed) | (pressed & allowed);

    // the d-pad and the stick share our direction bits; ImGui gets each as its own, the stick as analog
    const struct { unsigned bit; ImGuiKey key; } buttons[] = {
        {PAD_A, ImGuiKey_GamepadFaceDown}, {PAD_B, ImGuiKey_GamepadFaceRight},
        {PAD_UP, ImGuiKey_GamepadDpadUp},  {PAD_DOWN, ImGuiKey_GamepadDpadDown},
        {PAD_LEFT, ImGuiKey_GamepadDpadLeft}, {PAD_RIGHT, ImGuiKey_GamepadDpadRight},
    };
    for (const auto& b : buttons) io.AddKeyEvent(b.key, (g_fed & b.bit) != 0);
    // the left stick moves through ImGui as the d-pad does (both set the same bit, so the stick's own keys stay up:
    // ImGui would otherwise step twice for one push)
    const ImGuiKey lkeys[4] = {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft,
                               ImGuiKey_GamepadLStickRight};
    for (ImGuiKey k : lkeys) io.AddKeyAnalogEvent(k, false, 0.0f);
    // the right stick scrolls the page
    const ImGuiKey rkeys[4] = {ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickLeft,
                               ImGuiKey_GamepadRStickRight};
    for (int k = 0; k < 4; k++) io.AddKeyAnalogEvent(rkeys[k], nav && g_rs[k] > 0.1f, nav ? g_rs[k] : 0.0f);
}
