// The controller, read with XInput (as the games read it) in place of Dear ImGui's own reading, so the launcher
// decides which presses reach ImGui's navigation: none while another window (a game) is in front, none still held
// from before (the press that started the launcher from Quiver, or the game's last press), and only the buttons the
// page wants handed on. See main.cpp's HandleActions for what each button does.
#pragma once

enum PadButton : unsigned {
    PAD_UP = 1u << 0,  // the d-pad or the left stick
    PAD_DOWN = 1u << 1,
    PAD_LEFT = 1u << 2,
    PAD_RIGHT = 1u << 3,
    PAD_A = 1u << 4,
    PAD_B = 1u << 5,
    PAD_X = 1u << 6,
    PAD_Y = 1u << 7,
    PAD_LB = 1u << 8,
    PAD_RB = 1u << 9,
    PAD_START = 1u << 10,  // Menu on an Xbox pad
    PAD_BACK = 1u << 11,   // View on an Xbox pad
};

void pad_Init();
// Once a frame, before ImGui::NewFrame. live: the launcher is the front window. While it isn't, nothing reads, and
// whatever is held then must be let go before it counts.
void pad_Update(bool live);
// Buttons held now must be let go before they count (the window was away, or just came back).
void pad_Hold();
bool pad_Connected();
unsigned pad_Pressed();  // went down this frame
// Hands the controller to ImGui's navigation this frame: the d-pad, left stick and A when nav is set, B when cancel is
// set (a dropdown or text field closing), the right stick to scroll. A button only reaches ImGui if it went down while
// handed on, so a press that switched what has the focus doesn't also press what gets it.
void pad_Feed(bool nav, bool cancel);
