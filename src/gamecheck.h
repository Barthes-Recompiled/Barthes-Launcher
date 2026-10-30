// gamecheck.h - which disc a game folder or image holds, and checking a folder against that disc's known-good file
// list (gamecheck.c). Adapted from MiddleDucky's gamefiles.c (the PS and CoS launchers, MIT): the same lists and
// checks, for every game the Barthes Launcher knows instead of one.
#pragma once
#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GAMEFILE_OK, GAMEFILE_MISSING, GAMEFILE_SIZE, GAMEFILE_CONTENT };

#define GAMEFILES_LISTED 32  // the files named in a report; the counts cover all of them

typedef struct {
    const char* path;  // in the game folder, "EAMusic\\burrow.mus"
    uint32_t size, crc;
} GameFile;

typedef struct {
    const char* game;     // games.cpp id: "hp1", "hp2"
    const char* edition;  // "PAL", "NTSC-US"
    const GameFile* files;
    int count;
} DiscList;

typedef struct {
    volatile LONG64 done;   // bytes read (or, for a sizes-only check, files looked at) so far
    LONG64 total;           // bytes of all the listed files (files, for a sizes-only check)
    volatile LONG cancel;   // set from another thread: stops at the next block
    volatile LONG current;  // the file being read, an index into the list
    int missing, wrong_size, wrong_content;
    int listed;
    int index[GAMEFILES_LISTED];  // the first of them, in list order
    int kind[GAMEFILES_LISTED];   // GAMEFILE_*
    WCHAR error[320];             // why the check could not finish (cancelled, a read failing)
} GameCheck;

int disc_Count(void);
const DiscList* disc_Get(int i);

// CRC-32 as zlib computes it
uint32_t disc_Crc(uint32_t crc, const void* p, size_t n);

// The list whose default.xbe has these bytes, -1 for none (another game, or a disc nobody listed yet)
int disc_IdentifyXbe(const void* xbe, uint32_t size);
// The same for dir\default.xbe. A damaged default.xbe whose size still matches one list falls back to that list.
int disc_IdentifyFolder(const WCHAR* dir);

// Checks dir against list: every file there, its size, and with contents its CRC-32 too. Files the list does not
// name are left alone (saves, the player's own additions). FALSE when it could not finish (c->error).
BOOL disc_Check(const WCHAR* dir, int list, BOOL contents, GameCheck* c);

#ifdef __cplusplus
}
#endif
