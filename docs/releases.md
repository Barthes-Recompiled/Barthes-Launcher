# Publishing a game release the launcher can install

The launcher asks GitLab for each game's newest release
(`GET /api/v4/projects/barthes_recompiled%2Fhp1_recomp/releases?per_page=1`) and installs it if it finds:

1. **A `.zip` asset link.** If the release has several zips, the one with `windows` in its name wins, so name it
   like `hp1-recomp-windows-0.4.0.zip`. Link it under the project (a release upload or a generic package), so the link
   works without signing in.
2. **Optionally, `<zip name>.sha256`** beside it: the hex SHA-256 first on its line (`sha256sum` output is fine).
   With it, a damaged download is caught before anything is replaced.
3. **A tag like `v0.4.0`.** The launcher compares it with the `ProductVersion` in the installed
   `<x>_launcher.exe` (each repo's CMake `*_VERSION`) to offer updates, so keep the two in step.

The zip holds what `port/tools/package_release.sh` puts in `dist/`: `<x>_launcher.exe`, `<x>.exe`,
`shadercache\`, `README.txt`, `licenses\`. Files may sit at the top of the zip or in one folder; a `game\` folder in
the zip is ignored, so a release can never overwrite a player's disc.

The project must be public for players to see its releases (GitLab answers 404 to anyone signed out, which the
launcher shows as "no release yet").

The game exes the launcher expects, and the mutex it watches to tell whether a game is running:

| id | launcher | settings | running mutex |
|---|---|---|---|
| hp1 | `hpps_launcher.exe` | `hpps.toml` | `Local\hpps_game_running` |
| hp2 | `hpcos_launcher.exe` | `hpcos.toml` | `Local\hpcos_game_running` |
| hp3 | `hppoa_launcher.exe` | `hppoa.toml` | `Local\hppoa_game_running` |

When a game gets a new disc file list (`port/src/launcher/game_files*.inc` in its repo), copy it into `src/lists/`
and add it to `kLists` in `src/gamecheck.c`. Prisoner of Azkaban has no list yet, so its discs are accepted without
a check until one is made.
