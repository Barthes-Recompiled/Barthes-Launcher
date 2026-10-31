// games.h - the games the Barthes Launcher looks after, where each one lives, what state its folder is in, and the
// long jobs on a folder (install, unpack a disc, copy a disc folder, verify) that run on a thread of their own
#pragma once
#include <windows.h>

#include <string>

#include "gamecheck.h"
#include "net.h"
#include "xiso.h"

struct GameInfo {
    const char* id;          // "hp1": the section in barthes.ini, gamecheck.c's game
    const char* title;       // "Philosopher's Stone"
    const char* full_title;  // "Harry Potter and the Philosopher's Stone"
    const char* alt_title;   // the other region's name, or nullptr
    const char* year;        // the Xbox release
    const char* project;     // GitHub owner/repo: releases come from here
    const wchar_t* launcher; // the game's own launcher, which starts the game
    const wchar_t* toml;     // its settings file
    const wchar_t* mutex;    // held by the running game (each repo's single_instance.h)
    const char* stage;       // how far the port is, shown on its page
    bool playable;
    bool archipelago;        // the build connects to an Archipelago server (RECOMP_AP_SERVER / RECOMP_AP_SLOT)
    float hue;               // the accent on its spine and page
};

int games_Count();
const GameInfo& games_Info(int i);

// what is on disk for a game, refreshed by games_Refresh
struct GameState {
    std::wstring folder;      // where the build and game\ live
    bool custom_folder = false;
    bool installed = false;   // its launcher exe is there
    std::string version;      // the launcher exe's ProductVersion
    std::string build_edition;  // "PAL" / "USA": the disc the installed build is made for (barthes.ini), or empty
    bool has_disc = false;    // game\default.xbe
    bool has_image = false;   // game\*.iso waiting to be unpacked by the game's own launcher
    int disc = -1;            // gamecheck list of game\, -1 unknown
    bool wrong_game = false;  // game\ holds another game's disc
    bool running = false;
    // newest release on GitHub (filled in by the background check)
    bool release_checked = false;
    bool has_release = false;
    Release release;
    std::string release_error;
    // the last verification, from barthes.ini
    std::string verified;     // "2026-10-30, all 105 files whole" or empty
    // Archipelago (placeholder: saved, not yet passed to the game)
    std::string ap_server = "archipelago.gg:38281", ap_slot, ap_password;
    bool ap_enabled = false;
};

GameState& games_State(int i);

void games_Load();     // barthes.ini
void games_Save();
void games_Refresh(int i);
void games_RefreshAll();
void games_CheckReleasesAsync();  // all games' newest releases, on a thread
void games_Poll();                // once a frame: takes in finished release checks, notices a game starting or ending
bool games_UpdateAvailable(int i);
// the installed build is for the other edition of the disc in game\ (a reinstall picks the right one)
bool games_BuildMismatch(int i);
const char* games_DiscEdition(int i);  // "PAL" / "USA" for the disc in game\, or nullptr

// launcher-wide settings
enum AfterStart { AFTER_MINIMIZE, AFTER_CLOSE, AFTER_STAY };
int& games_AfterStart();

// starts the game's own launcher; settings=true makes it show its settings page this time even if the player turned
// that page off. FALSE with error when it could not.
bool games_Play(int i, bool settings, std::string& error);
void games_OpenFolder(int i, bool game_dir);
std::wstring games_ExeDir();

// ── long jobs, one at a time ────────────────────────────────────────────────────────────────────────────────────
enum JobKind { JOB_NONE, JOB_INSTALL, JOB_UNPACK, JOB_COPY, JOB_VERIFY };

struct Job {
    JobKind kind = JOB_NONE;
    int game = -1;
    volatile LONG running = 0;
    volatile LONG finished = 0;
    bool ok = false;
    char stage[256] = "";         // what it is doing now (written by the job's thread)
    std::string result;           // the outcome, for the page: read once finished is set
    Transfer transfer;            // download / copy / hash progress
    XisoProgress unpack{};        // disc image progress
    GameCheck check{};            // verification progress
};

Job& games_Job();
bool games_StartInstall(int i);
// source: a disc image (.iso/.xiso) or a game folder's default.xbe. FALSE with error when it is not this game's disc.
bool games_StartImport(int i, const std::wstring& source, std::string& error);
bool games_StartVerify(int i);
void games_CancelJob();
void games_FinishJob();  // after the page has shown the result
// 0..1 (negative while the size is unknown) and a line saying what is happening
float games_JobProgress(std::string& line);
