// gamecheck.c - telling discs apart and checking game folders (gamecheck.h)
//
// Each list (lists/*.inc, made by the game repos' tools/make_game_files.py) was taken from a folder that plays the
// whole game; an edition's files are the same bytes in every copy, so anything else is damage (a bad copy, a disc
// read error, a failed download) or a file someone changed. The lists and the check are MiddleDucky's, from the PS and
// CoS launchers; the Barthes Launcher only puts every game's lists side by side.
#include "gamecheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const GameFile kHp1Pal[] = {
#include "lists/hp1_pal.inc"
};
static const GameFile kHp1Usa[] = {
#include "lists/hp1_usa.inc"
};
static const GameFile kHp2Usa[] = {
#include "lists/hp2_usa.inc"
};
static const GameFile kHp2Eur[] = {
#include "lists/hp2_eur.inc"
};

#define LIST(game, edition, files) {game, edition, files, (int)(sizeof files / sizeof files[0])}
static const DiscList kLists[] = {
    LIST("hp1", "PAL", kHp1Pal),
    LIST("hp1", "NTSC-US", kHp1Usa),
    LIST("hp2", "NTSC-US", kHp2Usa),
    LIST("hp2", "PAL", kHp2Eur),
};
#define LIST_COUNT ((int)(sizeof kLists / sizeof kLists[0]))
#define READ_BUF   (4u << 20)

int disc_Count(void) { return LIST_COUNT; }
const DiscList* disc_Get(int i) { return i >= 0 && i < LIST_COUNT ? &kLists[i] : NULL; }

// reflected, polynomial 0xEDB88320, eight bytes a step
static uint32_t g_crc[8][256];

static void CrcInit(void) {
    if (g_crc[0][1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xFF];
}

uint32_t disc_Crc(uint32_t crc, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    CrcInit();
    crc = ~crc;
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^ g_crc[4][a >> 24] ^
              g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^ g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    }
    while (n--) crc = g_crc[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static const GameFile* XbeEntry(int list) {
    for (int i = 0; i < kLists[list].count; i++)
        if (!_stricmp(kLists[list].files[i].path, "default.xbe")) return &kLists[list].files[i];
    return NULL;
}

int disc_IdentifyXbe(const void* xbe, uint32_t size) {
    uint32_t crc = disc_Crc(0, xbe, size);
    for (int l = 0; l < LIST_COUNT; l++) {
        const GameFile* e = XbeEntry(l);
        if (e && e->size == size && e->crc == crc) return l;
    }
    return -1;
}

int disc_IdentifyFolder(const WCHAR* dir) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\default.xbe", dir);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    int found = -1;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20)) {
        uint32_t size = (uint32_t)sz.QuadPart;
        void* buf = malloc(size);
        DWORD got = 0;
        if (buf && ReadFile(h, buf, size, &got, NULL) && got == size) found = disc_IdentifyXbe(buf, size);
        free(buf);
        if (found < 0)  // a damaged default.xbe: its size still names the disc when only one list has it
            for (int l = 0, by_size = 0; l < LIST_COUNT; l++) {
                const GameFile* e = XbeEntry(l);
                if (e && e->size == size && !by_size++) found = l;
                else if (e && e->size == size) found = -1;
            }
    }
    CloseHandle(h);
    return found;
}

static void Note(GameCheck* c, int i, int kind) {
    if (kind == GAMEFILE_MISSING) c->missing++;
    else if (kind == GAMEFILE_SIZE) c->wrong_size++;
    else c->wrong_content++;
    if (c->listed < GAMEFILES_LISTED) {
        c->index[c->listed] = i;
        c->kind[c->listed++] = kind;
    }
}

// the file's CRC-32 into *crc; FALSE when it could not be read through
static BOOL ReadCrc(const WCHAR* path, uint32_t size, uint8_t* buf, GameCheck* c, uint32_t* crc) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    uint32_t v = 0;
    BOOL ok = TRUE;
    for (uint32_t left = size; ok && left;) {
        DWORD want = left < READ_BUF ? left : READ_BUF, got = 0;
        if (c->cancel) ok = FALSE;
        else if (!ReadFile(h, buf, want, &got, NULL) || got != want) ok = FALSE;
        else {
            v = disc_Crc(v, buf, got);
            left -= got;
            InterlockedAdd64(&c->done, got);
        }
    }
    CloseHandle(h);
    *crc = v;
    return ok;
}

BOOL disc_Check(const WCHAR* dir, int list, BOOL contents, GameCheck* c) {
    c->done = 0;
    c->total = 0;
    c->missing = c->wrong_size = c->wrong_content = c->listed = 0;
    c->error[0] = 0;
    if (list < 0 || list >= LIST_COUNT) {
        wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"This disc is not one the launcher has a list for.");
        return FALSE;
    }
    const GameFile* files = kLists[list].files;
    int count = kLists[list].count;
    for (int i = 0; i < count; i++) c->total += contents ? files[i].size : 1;
    uint8_t* buf = contents ? (uint8_t*)malloc(READ_BUF) : NULL;
    if (contents && !buf) {
        wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"Out of memory.");
        return FALSE;
    }
    BOOL ok = TRUE;
    for (int i = 0; ok && i < count; i++) {
        WCHAR name[MAX_PATH], path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fad;
        c->current = i;
        if (c->cancel) {
            wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"The check was cancelled.");
            ok = FALSE;
            break;
        }
        MultiByteToWideChar(CP_ACP, 0, files[i].path, -1, name, MAX_PATH);
        swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, name);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Note(c, i, GAMEFILE_MISSING);
            InterlockedAdd64(&c->done, contents ? files[i].size : 1);
            continue;
        }
        if (fad.nFileSizeHigh || fad.nFileSizeLow != files[i].size) {
            Note(c, i, GAMEFILE_SIZE);
            InterlockedAdd64(&c->done, contents ? files[i].size : 1);
            continue;
        }
        if (!contents) {
            InterlockedAdd64(&c->done, 1);
            continue;
        }
        uint32_t crc = 0;
        LONG64 before = c->done;
        if (!ReadCrc(path, files[i].size, buf, c, &crc)) {
            if (c->cancel) {
                wcscpy_s(c->error, sizeof c->error / sizeof c->error[0], L"The check was cancelled.");
                ok = FALSE;
            } else {  // there but unreadable: as damaged as a wrong byte
                Note(c, i, GAMEFILE_CONTENT);
                InterlockedExchange64(&c->done, before + files[i].size);
            }
        } else if (crc != files[i].crc) {
            Note(c, i, GAMEFILE_CONTENT);
        }
    }
    free(buf);
    return ok;
}
