// games.cpp - the game table, barthes.ini, folder state, starting a game and the long jobs (games.h)
#include "games.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

namespace {

// The ports themselves are MiddleDucky's (HP-PS-Xbox-Recomp, HP-COS-Xbox-Recomp) and the Barthes Recompiled forks of
// them; Play starts <x>.exe itself, with the environment the port expects (and the launcher variable that builds before
// the upstream launcher's removal check for).
const GameInfo kGames[] = {
    {"hp1", "Philosopher's Stone", "Harry Potter and the Philosopher's Stone", "Sorcerer's Stone", "2003",
     "Barthes-Recompiled/HP1-Recompiled", L"hpps.exe", L"HPPS_LAUNCHER", L"hpps.toml", L"Local\\hpps_game_running",
     "Playable from start to finish", true, true, "PAL", 0.0f},
    {"hp2", "Chamber of Secrets", "Harry Potter and the Chamber of Secrets", nullptr, "2002",
     "Barthes-Recompiled/HP2-Recompiled", L"hpcos.exe", L"HPCOS_LAUNCHER", L"hpcos.toml", L"Local\\hpcos_game_running",
     "Playable from start to finish", true, false, "USA", 0.38f},
    {"hp3", "Prisoner of Azkaban", "Harry Potter and the Prisoner of Azkaban", nullptr, "2004",
     "Barthes-Recompiled/HP3-Recompiled", L"hppoa.exe", L"HPPOA_LAUNCHER", L"hppoa.toml", L"Local\\hppoa_game_running",
     "Beta: playable from the Hogwarts Express on", true, false, "USA", 0.62f},
};
constexpr int kCount = int(std::size(kGames));

GameState g_state[kCount];
std::string g_project[kCount];  // where releases come from: the game's project, or barthes.ini's release_project
int g_after_start = AFTER_MINIMIZE;
int g_pad_labels = LABELS_XBOX;
bool g_pad_hints = true;
Job g_job;
HANDLE g_job_thread = nullptr;

// release checks land here from their thread; games_Poll copies them in
struct ReleaseResult {
    volatile LONG ready = 0;
    bool has = false;
    Release r;
    std::string error;
} g_releases[kCount];

std::wstring IniPath() { return games_ExeDir() + L"barthes.ini"; }

std::string IniGet(const char* section, const char* key, const char* def = "") {
    wchar_t buf[2048];
    GetPrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(def).c_str(), buf, 2048, IniPath().c_str());
    return Narrow(buf);
}

void IniSet(const char* section, const char* key, const std::string& v) {
    WritePrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(v).c_str(), IniPath().c_str());
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ProductVersion(const std::wstring& exe) {
    DWORD handle = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (!size) return {};
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    wchar_t* v = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\StringFileInfo\\040904B0\\ProductVersion", (void**)&v, &len) && len)
        return Narrow(v);
    VS_FIXEDFILEINFO* fi = nullptr;
    if (VerQueryValueW(data.data(), L"\\", (void**)&fi, &len) && fi) {
        char buf[64];
        snprintf(buf, sizeof buf, "%u.%u.%u", HIWORD(fi->dwProductVersionMS), LOWORD(fi->dwProductVersionMS),
                 HIWORD(fi->dwProductVersionLS));
        return buf;
    }
    return {};
}

// "0.10.1" > "0.9.3"
int CompareVersions(const std::string& a, const std::string& b) {
    const char *p = a.c_str(), *q = b.c_str();
    while (*p || *q) {
        long x = strtol(p, (char**)&p, 10), y = strtol(q, (char**)&q, 10);
        if (x != y) return x < y ? -1 : 1;
        while (*p && !isdigit((unsigned char)*p)) p++;
        while (*q && !isdigit((unsigned char)*q)) q++;
    }
    return 0;
}

std::string Today() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d", &lt);
    return buf;
}

void SetStage(const char* s) {
    strncpy(g_job.stage, s, sizeof g_job.stage - 1);
    g_job.stage[sizeof g_job.stage - 1] = 0;
}

std::string GameDir(int i) { return Narrow(g_state[i].folder + L"\\game"); }

// ── jobs ────────────────────────────────────────────────────────────────────────────────────────────────────────

// moves src's contents into dst, replacing what is there; a top-level game\ in the release is left out (the
// player's disc lives there)
bool MoveTree(const std::wstring& src, const std::wstring& dst, bool top, std::string& error) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (top && _wcsicmp(name.c_str(), L"game") == 0) continue;
        std::wstring s = src + L"\\" + name, d = dst + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = MoveTree(s, d, false, error);
        } else {
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!MoveFileExW(s.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                error = "Could not put " + Narrow(name) + " in place (is the game still running?)";
                ok = false;
            }
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring p = dir + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                RemoveTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(p.c_str());  // a junction: never into it
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Windows 10 and 11 ship bsdtar, which reads .zip
bool Unzip(const std::wstring& zip, const std::wstring& dir, std::string& error) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (!Exists(tar)) {
        error = "Windows' tar.exe was not found, so the download could not be unpacked.";
        return false;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        error = "Could not run tar.exe to unpack the download.";
        return false;
    }
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        if (g_job.transfer.cancel) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code) error = g_job.transfer.cancel ? "Cancelled." : "The download could not be unpacked (tar.exe failed).";
    return code == 0;
}

// a release zip holds the files either at its top or in one folder
std::wstring ReleaseRoot(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return dir;
    int dirs = 0, files = 0;
    std::wstring only;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs++; only = n; }
        else files++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return dirs == 1 && !files ? dir + L"\\" + only : dir;
}

// a game built per disc edition attaches one zip each, named with PAL or USA; pick the one for the disc in
// game\ (before a disc is added: the first, the port's own edition)
const char* EditionTag(const char* edition) { return strcmp(edition, "PAL") == 0 ? "PAL" : "USA"; }

const ReleaseZip* PickZip(int i, std::string& edition) {
    const GameState& st = g_state[i];
    edition.clear();
    const char* want = games_DiscEdition(i);
    if (!want) want = kGames[i].edition;
    const ReleaseZip* first = nullptr;
    for (const ReleaseZip& z : st.release.zips) {
        std::string lower = z.name;
        for (char& c : lower) c = char(tolower((unsigned char)c));
        if (lower.find("windows") == std::string::npos) continue;
        const char* tag = lower.find("-pal") != std::string::npos ? "PAL" : lower.find("-usa") != std::string::npos ? "USA" : "";
        if (!first) { first = &z; edition = tag; }
        if (want && strcmp(tag, want) == 0) { edition = tag; return &z; }
    }
    return first;
}

bool RunInstall(int i, std::string& result) {
    GameState& st = g_state[i];
    Release r = st.release;
    std::string edition;
    if (const ReleaseZip* z = PickZip(i, edition)) {
        r.zip_name = z->name;
        r.zip_url = z->url;
        r.sha256_url = z->sha256_url;
    }
    std::wstring work = st.folder + L"\\.barthes";
    SHCreateDirectoryExW(nullptr, work.c_str(), nullptr);
    std::wstring zip = work + L"\\" + Widen(r.zip_name);
    std::string error;

    SetStage(("Downloading " + r.zip_name).c_str());
    if (!net_Download(Widen(r.zip_url), zip, &g_job.transfer, error)) { result = error; return false; }

    if (!r.sha256_url.empty()) {
        SetStage("Checking the download");
        std::string want;
        if (!net_Get(Widen(r.sha256_url), want, error)) { result = error; return false; }
        want = want.substr(0, want.find_first_of(" \t\r\n"));
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        g_job.transfer.done = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fad))
            g_job.transfer.total = (LONG64(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        std::string got = net_Sha256File(zip, &g_job.transfer);
        if (got != want) {
            DeleteFileW(zip.c_str());
            result = "The download was damaged (its SHA-256 does not match the release). Please try again.";
            return false;
        }
    }

    SetStage("Unpacking");
    g_job.transfer.total = 0;
    std::wstring staging = work + L"\\staging";
    RemoveTree(staging);
    if (!Unzip(zip, staging, error)) { result = error; return false; }
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kGames[i].mutex)) {
        CloseHandle(m);
        result = std::string(kGames[i].title) + " is running. Close it, then install again.";
        return false;
    }
    SetStage("Putting the files in place");
    if (!MoveTree(ReleaseRoot(staging), st.folder, true, error)) { result = error; return false; }
    RemoveTree(work);
    SHCreateDirectoryExW(nullptr, (st.folder + L"\\game").c_str(), nullptr);
    IniSet(kGames[i].id, "build_edition", edition);
    st.build_edition = edition;
    result = "Installed " + r.tag + (edition.empty() ? "" : " for the " + edition + " disc") + ".";
    return true;
}

// a game folder elsewhere copied into game\, files already there whole kept
struct CopyItem {
    std::wstring rel;
    ULONGLONG size;
};

void ListTree(const std::wstring& root, const std::wstring& rel, std::vector<CopyItem>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListTree(root, rel + L"\\" + n, out);
        else out.push_back({rel + L"\\" + n, (ULONGLONG(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER copied, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE,
                            HANDLE, LPVOID base) {
    g_job.transfer.done = *(LONG64*)base + copied.QuadPart;
    return g_job.transfer.cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool RunCopy(const std::wstring& src, const std::wstring& dst, std::string& result) {
    std::vector<CopyItem> items;
    ListTree(src, L"", items);
    g_job.transfer.total = 0;
    for (auto& it : items) g_job.transfer.total += it.size;
    LONG64 base = 0;
    // default.xbe last, so a stopped copy never looks like a whole folder
    std::stable_partition(items.begin(), items.end(), [](const CopyItem& c) { return _wcsicmp(c.rel.c_str(), L"\\default.xbe") != 0; });
    for (auto& it : items) {
        std::wstring s = src + it.rel, d = dst + it.rel;
        SHCreateDirectoryExW(nullptr, d.substr(0, d.find_last_of(L'\\')).c_str(), nullptr);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        bool whole = GetFileAttributesExW(d.c_str(), GetFileExInfoStandard, &fad) &&
                     ((ULONGLONG(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) == it.size;
        if (!whole) {
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!CopyFileExW(s.c_str(), d.c_str(), CopyProgress, &base, nullptr, 0)) {
                result = g_job.transfer.cancel ? "Cancelled." : "Could not copy " + Narrow(it.rel.substr(1)) + ".";
                return false;
            }
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);  // copies off a disc come read-only
        }
        base += it.size;
        g_job.transfer.done = base;
    }
    return true;
}

std::string CheckSummary(const GameCheck& c, int list) {
    const DiscList* dl = disc_Get(list);
    char buf[256];
    if (!c.missing && !c.wrong_size && !c.wrong_content) {
        snprintf(buf, sizeof buf, "All %d files of the %s disc are whole.", dl->count, dl->edition);
        return buf;
    }
    snprintf(buf, sizeof buf, "%d missing, %d the wrong size, %d damaged (of %d files on the %s disc).", c.missing,
             c.wrong_size, c.wrong_content, dl->count, dl->edition);
    std::string s = buf;
    for (int k = 0; k < c.listed && k < 4; k++) s += std::string("\n  ") + dl->files[c.index[k]].path;
    if (c.listed > 4) s += "\n  ...";
    return s;
}

// Prisoner of Azkaban has no file list yet: its discs are known by default.xbe alone, so there is nothing to verify
bool HasList(int i) {
    for (int k = 0; const DiscList* d = disc_Get(k); k++)
        if (!strcmp(d->game, kGames[i].id)) return true;
    return false;
}

bool RunVerify(int i, std::string& result) {
    std::wstring dir = g_state[i].folder + L"\\game";
    int list = disc_IdentifyFolder(dir.c_str());
    if (list < 0 || strcmp(disc_Get(list)->game, kGames[i].id) != 0) {
        result = "game\\default.xbe is not a disc the launcher knows for this game, so there is nothing to check it against.";
        return false;
    }
    SetStage("Reading every file of the disc");
    if (!disc_Check(dir.c_str(), list, TRUE, &g_job.check)) {
        result = Narrow(g_job.check.error);
        return false;
    }
    result = CheckSummary(g_job.check, list);
    bool whole = !g_job.check.missing && !g_job.check.wrong_size && !g_job.check.wrong_content;
    IniSet(kGames[i].id, "verified", whole ? Today() + ": " + disc_Get(list)->edition + " disc, all files whole" : "");
    return whole;
}

DWORD WINAPI JobThread(void*) {
    int i = g_job.game;
    std::string result;
    bool ok = false;
    switch (g_job.kind) {
    case JOB_INSTALL: ok = RunInstall(i, result); break;
    case JOB_UNPACK: {
        std::wstring dir = g_state[i].folder + L"\\game";
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        SetStage("Unpacking the disc image");
        ok = xiso_Extract(Widen(g_job.result).c_str(), dir.c_str(), &g_job.unpack) != FALSE;
        result = ok ? "" : Narrow(g_job.unpack.error);
        if (ok && HasList(i)) { g_job.kind = JOB_VERIFY; ok = RunVerify(i, result); }
        break;
    }
    case JOB_COPY: {
        std::wstring dir = g_state[i].folder + L"\\game";
        SetStage("Copying the game files");
        ok = RunCopy(Widen(g_job.result), dir, result);
        if (ok && HasList(i)) { g_job.kind = JOB_VERIFY; ok = RunVerify(i, result); }
        break;
    }
    case JOB_VERIFY: ok = RunVerify(i, result); break;
    default: break;
    }
    g_job.result = result;
    g_job.ok = ok;
    InterlockedExchange(&g_job.finished, 1);
    return 0;
}

bool StartJob(JobKind kind, int i, const std::string& arg = {}) {
    if (g_job.running) return false;
    if (g_job_thread) { CloseHandle(g_job_thread); g_job_thread = nullptr; }
    g_job.kind = kind;
    g_job.game = i;
    g_job.ok = false;
    g_job.finished = 0;
    g_job.stage[0] = 0;
    g_job.result = arg;  // the source path for unpack / copy, replaced by the outcome
    g_job.transfer.done = 0;
    g_job.transfer.total = 0;
    g_job.transfer.cancel = 0;
    g_job.unpack = XisoProgress{};
    g_job.check = GameCheck{};
    g_job.running = 1;
    g_job_thread = CreateThread(nullptr, 0, JobThread, nullptr, 0, nullptr);
    if (!g_job_thread) { g_job.running = 0; return false; }
    return true;
}

DWORD WINAPI ReleaseThread(void*) {
    for (int i = 0; i < kCount; i++) {
        ReleaseResult& rr = g_releases[i];
        rr.has = net_LatestRelease(g_project[i], rr.r, rr.error);
        InterlockedExchange(&rr.ready, 1);
    }
    return 0;
}

}  // namespace

int games_Count() { return kCount; }
const GameInfo& games_Info(int i) { return kGames[i]; }
GameState& games_State(int i) { return g_state[i]; }
int& games_AfterStart() { return g_after_start; }
int& games_PadLabels() { return g_pad_labels; }
bool& games_PadHints() { return g_pad_hints; }
Job& games_Job() { return g_job; }

std::wstring games_ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

void games_Load() {
    std::string after = IniGet("launcher", "after_start", "minimize");
    g_after_start = after == "close" ? AFTER_CLOSE : after == "stay" ? AFTER_STAY : AFTER_MINIMIZE;
    std::string labels = IniGet("launcher", "pad_labels", "xbox");
    g_pad_labels = labels == "playstation" ? LABELS_PLAYSTATION : labels == "nintendo" ? LABELS_NINTENDO : LABELS_XBOX;
    g_pad_hints = IniGet("launcher", "pad_hints", "true") != "false";
    for (int i = 0; i < kCount; i++) {
        GameState& s = g_state[i];
        const char* id = kGames[i].id;
        std::string folder = IniGet(id, "folder");
        s.custom_folder = !folder.empty();
        s.folder = folder.empty() ? games_ExeDir() + L"games\\" + Widen(id) : Widen(folder);
        while (!s.folder.empty() && (s.folder.back() == L'\\' || s.folder.back() == L'/')) s.folder.pop_back();
        s.verified = IniGet(id, "verified");
        // another project's releases (a fork, a test channel)
        g_project[i] = IniGet(id, "release_project", kGames[i].project);
        s.ap_server = IniGet(id, "ap_server", "archipelago.gg:38281");
        s.ap_slot = IniGet(id, "ap_slot");
        s.ap_password = IniGet(id, "ap_password");
        s.ap_enabled = IniGet(id, "ap_enabled", "false") == "true";
    }
}

void games_Save() {
    const char* after[] = {"minimize", "close", "stay"};
    IniSet("launcher", "after_start", after[g_after_start]);
    const char* labels[] = {"xbox", "playstation", "nintendo"};
    IniSet("launcher", "pad_labels", labels[g_pad_labels]);
    IniSet("launcher", "pad_hints", g_pad_hints ? "true" : "false");
    for (int i = 0; i < kCount; i++) {
        GameState& s = g_state[i];
        const char* id = kGames[i].id;
        IniSet(id, "folder", s.custom_folder ? Narrow(s.folder) : "");
        IniSet(id, "ap_server", s.ap_server);
        IniSet(id, "ap_slot", s.ap_slot);
        IniSet(id, "ap_password", s.ap_password);
        IniSet(id, "ap_enabled", s.ap_enabled ? "true" : "false");
    }
}

void games_Refresh(int i) {
    GameState& s = g_state[i];
    const GameInfo& g = kGames[i];
    std::wstring exe = s.folder + L"\\" + g.exe;
    s.installed = Exists(exe);
    s.version = s.installed ? ProductVersion(exe) : "";
    std::wstring game = s.folder + L"\\game";
    s.has_disc = Exists(game + L"\\default.xbe");
    wchar_t image[MAX_PATH];
    s.has_image = !s.has_disc && IsDir(game) && xiso_Find(game.c_str(), image);
    s.disc = s.has_disc ? disc_IdentifyFolder(game.c_str()) : -1;
    // a game without file lists: its edition from default.xbe's SHA-1 (the port's port/region/*.json)
    s.xbe_edition.clear();
    if (s.has_disc && s.disc < 0) {
        static const struct { const char *game, *sha1, *edition; } kXbes[] = {
            {"hp3", "51b09e01169fda86f2cb7e5ce45ee80a10268918", "USA"},
            {"hp3", "c8841e3c1d2d5ec20dbe286e5bec9bb5104b41f9", "PAL"},
        };
        std::string sha = net_Sha1File(game + L"\\default.xbe");
        for (const auto& x : kXbes)
            if (strcmp(x.game, g.id) == 0 && sha == x.sha1) s.xbe_edition = x.edition;
    }
    s.wrong_game = s.disc >= 0 && strcmp(disc_Get(s.disc)->game, g.id) != 0;
    s.verified = IniGet(g.id, "verified");
    s.build_edition = s.installed ? IniGet(g.id, "build_edition") : "";
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, g.mutex);
    s.running = m != nullptr;
    if (m) CloseHandle(m);
}

void games_RefreshAll() {
    for (int i = 0; i < kCount; i++) games_Refresh(i);
}

void games_CheckReleasesAsync() {
    for (auto& r : g_releases) r.ready = 0;
    HANDLE t = CreateThread(nullptr, 0, ReleaseThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void games_Poll() {
    for (int i = 0; i < kCount; i++) {
        ReleaseResult& rr = g_releases[i];
        if (rr.ready == 1) {
            GameState& s = g_state[i];
            s.release_checked = true;
            s.has_release = rr.has;
            s.release = rr.r;
            s.release_error = rr.error;
            rr.ready = 2;
        }
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kGames[i].mutex);
        bool running = m != nullptr;
        if (m) CloseHandle(m);
        if (running != g_state[i].running) games_Refresh(i);
    }
}

bool games_UpdateAvailable(int i) {
    const GameState& s = g_state[i];
    return s.installed && s.has_release && !s.version.empty() && CompareVersions(s.release.version, s.version) > 0;
}

const char* games_DiscEdition(int i) {
    const GameState& s = g_state[i];
    if (s.disc >= 0 && !s.wrong_game) return EditionTag(disc_Get(s.disc)->edition);
    return s.xbe_edition.empty() ? nullptr : s.xbe_edition.c_str();
}

bool games_BuildMismatch(int i) {
    const GameState& s = g_state[i];
    const char* disc = games_DiscEdition(i);
    return s.installed && disc && !s.build_edition.empty() && s.build_edition != disc;
}

bool games_Play(int i, bool settings, std::string& error) {
    const GameInfo& g = kGames[i];
    GameState& s = g_state[i];
    games_Refresh(i);
    if (s.running) {
        error = std::string(g.title) + " is already running.";
        return false;
    }
    if (!s.installed) {
        error = "The game isn't installed yet.";
        return false;
    }
    // the game itself, with the environment the port expects (anything the player set already wins; current builds set
    // the same defaults themselves), and the variable builds up to 1.0.1 / 0.5.1 check to know a launcher started them
    const std::pair<const wchar_t*, const wchar_t*> kEnv[] = {
        {L"RECOMP_TITLE_KEVENTS", L"1"}, {L"RECOMP_VBLANK", L"1"},       {L"RECOMP_AC97_READY", L"1"},
        {L"RECOMP_DSP_ACK", L"0x834D0810"}, {L"RECOMP_PB_EXEC", L"1"}, {L"RECOMP_FB_WINDOW", L"1"},
        {L"RECOMP_USB", L"1"},           {L"RECOMP_ACK_SLEEP_US", L"250"},
    };
    for (auto [k, v] : kEnv)
        if (!GetEnvironmentVariableW(k, nullptr, 0)) SetEnvironmentVariableW(k, v);
    // Philosopher's Stone and Prisoner of Azkaban give freed memory back, as their launchers ask; Chamber of Secrets
    // doesn't
    SetEnvironmentVariableW(L"RECOMP_HEAP_RECLAIM", strcmp(g.id, "hp2") != 0 ? L"1" : nullptr);
    // Game settings: the port's settings page before the game starts (F10's page, with "Start game")
    SetEnvironmentVariableW(L"RECOMP_START_MENU", settings ? L"1" : L"0");
    SetEnvironmentVariableW(g.launcher_env, L"1");
    // Archipelago: the game connects when RECOMP_AP_SERVER and RECOMP_AP_SLOT are set (its own launcher passes them
    // on); the password goes in saves\archipelago\connection.toml so it never shows on a command line (the game
    // reads it between the first and the last quote, unescaped)
    if (g.archipelago && s.ap_enabled && !s.ap_server.empty() && !s.ap_slot.empty()) {
        SetEnvironmentVariableW(L"RECOMP_AP_SERVER", Widen(s.ap_server).c_str());
        SetEnvironmentVariableW(L"RECOMP_AP_SLOT", Widen(s.ap_slot).c_str());
        std::wstring dir = s.folder + L"\\saves\\archipelago";
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (FILE* f = _wfopen((dir + L"\\connection.toml").c_str(), L"wb")) {
            fprintf(f, "password = \"%s\"\n", s.ap_password.c_str());
            fclose(f);
        }
    } else {
        SetEnvironmentVariableW(L"RECOMP_AP_SERVER", nullptr);
        SetEnvironmentVariableW(L"RECOMP_AP_SLOT", nullptr);
    }
    std::wstring exe = s.folder + L"\\" + g.exe;
    std::wstring cmd = L"\"" + exe + L"\"";
    // the game's console output (its [CRASH] reports among it) to <game>_run.log beside it, for a problem report: a
    // windowed launcher has no console to hand down
    std::wstring log_path = exe.substr(0, exe.size() - 4) + L"_run.log";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof si};
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, s.folder.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        error = "Could not start " + Narrow(g.exe) + ".";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void games_OpenFolder(int i, bool game_dir) {
    std::wstring dir = g_state[i].folder + (game_dir ? L"\\game" : L"");
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool games_StartInstall(int i) {
    if (!g_state[i].has_release) return false;
    SHCreateDirectoryExW(nullptr, g_state[i].folder.c_str(), nullptr);
    return StartJob(JOB_INSTALL, i);
}

bool games_StartImport(int i, const std::wstring& source, std::string& error) {
    const GameInfo& g = kGames[i];
    size_t dot = source.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : source.substr(dot);
    int list = -1;
    bool image = _wcsicmp(ext.c_str(), L".iso") == 0 || _wcsicmp(ext.c_str(), L".xiso") == 0;
    std::wstring folder;
    if (image) {
        std::vector<char> buf(32u << 20);
        DWORD size = 0;
        if (!xiso_ReadRootFile(source.c_str(), "default.xbe", buf.data(), DWORD(buf.size()), &size)) {
            error = "That file is not a readable Xbox disc image.";
            return false;
        }
        list = disc_IdentifyXbe(buf.data(), size);
    } else {
        folder = source.substr(0, source.find_last_of(L"\\/"));
        list = disc_IdentifyFolder(folder.c_str());
        if (_wcsicmp((folder + L"\\").c_str(), (g_state[i].folder + L"\\game\\").c_str()) == 0) {
            error = "That is already this game's folder.";
            return false;
        }
    }
    if (list >= 0 && strcmp(disc_Get(list)->game, g.id) != 0) {
        error = std::string("That is the ") + disc_Get(list)->edition + " disc of " +
                kGames[disc_Get(list)->game[2] - '1'].full_title + ", not " + g.full_title + ".";
        return false;
    }
    if (list < 0 && strcmp(g.id, "hp3") != 0) {
        error = std::string("That disc doesn't match any known ") + g.title +
                " disc (PAL or NTSC-US). A modified or damaged dump won't work with the recomp.";
        return false;
    }
    SHCreateDirectoryExW(nullptr, (g_state[i].folder + L"\\game").c_str(), nullptr);
    return StartJob(image ? JOB_UNPACK : JOB_COPY, i, Narrow(image ? source : folder));
}

bool games_StartVerify(int i) { return StartJob(JOB_VERIFY, i); }

void games_CancelJob() {
    InterlockedExchange(&g_job.transfer.cancel, 1);
    InterlockedExchange(&g_job.unpack.cancel, 1);
    InterlockedExchange(&g_job.check.cancel, 1);
}

void games_FinishJob() {
    if (g_job_thread) {
        WaitForSingleObject(g_job_thread, INFINITE);
        CloseHandle(g_job_thread);
        g_job_thread = nullptr;
    }
    int i = g_job.game;
    g_job.running = 0;
    g_job.finished = 0;
    g_job.kind = JOB_NONE;
    if (i >= 0) games_Refresh(i);
}

float games_JobProgress(std::string& line) {
    char buf[320];
    double done = 0, total = 0;
    switch (g_job.kind) {
    case JOB_UNPACK: done = double(g_job.unpack.done); total = double(g_job.unpack.total); break;
    case JOB_VERIFY: done = double(g_job.check.done); total = double(g_job.check.total); break;
    default: done = double(g_job.transfer.done); total = double(g_job.transfer.total); break;
    }
    if (total > 0) snprintf(buf, sizeof buf, "%s  -  %.0f of %.0f MB", g_job.stage, done / 1e6, total / 1e6);
    else if (done > 0) snprintf(buf, sizeof buf, "%s  -  %.0f MB", g_job.stage, done / 1e6);
    else snprintf(buf, sizeof buf, "%s", g_job.stage);
    line = buf;
    return total > 0 ? float(std::min(1.0, done / total)) : -1.0f;
}
