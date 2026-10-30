# Barthes Launcher

One home for the Barthes Recompiled ports of the original Xbox Harry Potter games:

| | Game | Port | State |
|---|---|---|---|
| I | Harry Potter and the Philosopher's Stone (Sorcerer's Stone) | [hp1_recomp](https://gitlab.com/barthes_recompiled/hp1_recomp) | Playable |
| II | Harry Potter and the Chamber of Secrets | [hp2_recomp](https://gitlab.com/barthes_recompiled/hp2_recomp) | Playable |
| III | Harry Potter and the Prisoner of Azkaban | [hp3_recomp](https://gitlab.com/barthes_recompiled/hp3_recomp) | In development |

The games stand as books on a shelf. Pick one and its page lets you:

- **Install or update** the port from its GitLab releases (the download is checked against its SHA-256 before anything
  is replaced; your `game\` folder, saves and settings are never touched).
- **Add your disc**: an `.iso`/`.xiso` image is unpacked, an extracted game folder is copied. The launcher reads the
  disc's `default.xbe` first and refuses another game's disc, so a Chamber of Secrets image can't end up in the
  Philosopher's Stone folder.
- **Verify files** against the known-good list of every file on the PAL and NTSC-US discs.
- **Play**, which opens that game's own launcher (resolution, frame rate, controls and the rest live there), or
  **Game settings**, which opens it on its settings page even if you told it not to show at startup.
- **Archipelago**: a page per game for the multiworld randomizer. It is a placeholder for now; the fields are saved
  and will be wired up when each game's Archipelago support lands.

![The Play page](docs/screenshots/play.png)

You need your own copy of the games. Nothing from the games is included here or in any release.

## Using it

1. Download `barthes_launcher.exe` from the [releases](https://gitlab.com/barthes_recompiled/barthes-launcher/-/releases)
   and put it in a folder of its own, for example `C:\Games\Barthes`.
2. Start it, pick a book, press **Install**.
3. Press **Add disc image or folder...** and choose your `.iso`, or the `default.xbe` of a disc you already extracted.
4. Press **Play**.

Each game lives in `games\hp1`, `games\hp2`, `games\hp3` beside the launcher, with its disc in `game\` inside that.
Already have a port unpacked somewhere else? **Use another folder...** points the launcher at it instead.
The launcher remembers its choices in `barthes.ini` beside it.

Windows 10 (1809) or later, 64-bit. The launcher uses only what Windows ships (WinHTTP, `tar.exe` for the release
zips, Direct3D 11).

## Building

MSYS2 UCRT64, the same toolchain as the games:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`tools/package.sh` builds the release zip and its `.sha256` into `dist/`. For testing, the launcher takes
`--page <hp1|hp2|hp3|colophon>`, `--tab archipelago`, `--verify 1`, `--import <iso or default.xbe>`, `--install 1`
and `--play 1`, and `barthes.ini` takes `release_project = group/project` in a game's section to install from
another GitLab project (a fork, a test channel).

How a game repo publishes a release the launcher can install: [docs/releases.md](docs/releases.md).

## Thanks

**MiddleDucky**, above all. Their Xbox ports of
[Philosopher's Stone](https://github.com/MiddleDucky/HP-PS-Xbox-Recomp) and
[Chamber of Secrets](https://github.com/MiddleDucky/HP-COS-Xbox-Recomp) are the ground every book on this shelf stands
on: the recompiler work, the renderer, the audio and input, the launchers, and the fixes the Prisoner of Azkaban port is
being built from. This launcher's disc unpacking (`src/xiso.c`) and file checks (`src/gamecheck.c`, `src/lists/`) are
their code, put to work for all three games at once. None of this exists without them. Thank you, MiddleDucky, truly
and enormously.

**Athelas**, whose launcher showed what a good home for a recomp can be, and set the bar for what this one should do.
The look here is our own; the inspiration for doing it properly is theirs.

[Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut draws the window.

## Why Barthes

> "To give a text an Author is to impose a limit on that text."
>
> Roland Barthes

"I named this project after Roland Barthes to express - in whatever way I could - that my affection for these games has nothing to do with the author of the books on which they are based."

## License

MIT, see [LICENSE](LICENSE). The files taken from MiddleDucky's ports keep their MIT notice, and Dear ImGui keeps its
own (`third_party/imgui/LICENSE.txt`). The games and everything on their discs belong to their owners.
