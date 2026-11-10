# Apotris Splitscreen: design and test notes

Unofficial fork of Apotris v4.2.0 (upstream: <https://gitea.com/akouzoukos/apotris>). It adds local
two-player split screen to the Linux PC build: one window, two full boards, two pads, no internet.

## Using it

- **Play > Multi Battle > Split Screen.** Player 2 appears on the right half, already in the battle: it
  never shows a title screen or menus, and both players land in the lobby by themselves. Ready up,
  battle, results and Play Again all stay split.
- The moment player 1 goes to any other scene (B out of the lobby, Main Menu on the results, Play While
  Waiting, Cancel Queue), player 2 saves and closes, and player 1 is back on one screen.
- `Apotris --split` starts straight in a split screen battle.
- The first two controllers are player 1 and player 2. The keyboard goes to player 1 (or to the first
  player without a pad); F8 hands it to the other player.
- Single player takes input from every controller, not only the first.

## Design: one window, two game processes

```
Apotris  (player 1; while split: owns the window, music and every input device)
   |  fork/exec when Split Screen is picked; inherits a memfd and two socketpairs
   v
Apotris --split-child  (player 2: no real window, own save, renders offscreen)

P2 framebuffer  -> memfd triple buffer -> player 1 draws it on the right half
P2's pad events -> control socket      -> player 2's pressKey()/unpressKey()
Battle packets  -> link socket         -> LocalLink on both sides
```

Why processes and not two boards in one process:

- `Game` is a clean class (CPU Battle already runs two), but what draws it is not. `source/game.cpp`
  keeps gameplay and effect state in file-level globals and draws to one set of 4 tile layers, 128
  sprites and shared palettes. Shake, zone flashes and palette effects are whole-screen.
- A real in-process split means moving all of that into a per-player context and rewriting the blocking
  menus: weeks of work that conflicts with every upstream release.
- A process boundary gives the same result on screen for a few hundred lines, because every effect is
  already isolated. It also gives each player their own menus, skin and handling settings.
- The cost is two audio clients and up to one frame of extra latency on player 2's picture (not on its
  input handling).

All platform output goes through `updateWindow()` in `source/liba_window.cpp`, the framebuffer is a fixed
512x512 RGBA buffer, and all input goes through `handleInput()`.

## How it works

- **Start and stop** (`split::enable()` / `disable()`, `source/splitScreen.cpp`). Player 1 already has
  threads, so the child is forked with its arguments built beforehand and only async-signal-safe calls
  before `exec`. `close_range(CLOSE_RANGE_CLOEXEC)` keeps player 1's pads, audio and display connection
  out of it. If the binary was replaced while running, the child runs from `/proc/self/exe`.
- **One battle.** `switchScene()` (`source/menuSystem.cpp`) closes player 2 when player 1 switches to a
  scene outside the battle (`partOfABattle()`: queue, MATCH FOUND, lobby, games, results). Player 2 can
  leave for its own menus and rejoin with Multi Battle; only player 1 closes it. If player 2 quits or
  crashes, player 1 goes back to one screen once any battle with it has ended.
- **Boot into the battle.** Player 2 always, and player 1 with `--split`, skip the title and go straight
  to `MatchmakingQueue::startLocal()`; `loadMenuBasics()` does the title's setup for the menus. A side
  waiting in the queue screen joins as soon as both are in, so nobody is asked MATCH FOUND.
- **Layout** (`onSplitScreenChanged()`, `source/liba_window.cpp`). Fullscreen while split, whatever the
  save says, then back to the save's setting. Each half fits the whole 240x160 screen in menus and zooms
  to 192 columns while playing (4x to 5x at 1080p). Each side picks its own scale (player 2 reports its
  scale to player 1), and the background gradient is built for the fit scale's rows on both sides. Both
  the SDL renderer and the GL shader path draw both halves.
- **Input.** Player 1 routes player 2's pad and keyboard events over the control socket and ignores key
  repeats (a repeat could press a key on player 1 whose release goes to player 2). Player 2 ignores
  anything its own SDL picks up. Pad events reach both players even when the window is not focused.
- **Saves.** Player 2 saves to `~/.local/share/Apotris-p2/`, seeded once from player 1's save (and a
  link to its `skins/`), and works in that folder, so `replay.sav` is separate too.
  Player 1's save is never touched by player 2. Player 2 is named `player2`.
- **Audio.** Player 2 plays sound effects with its music muted, and never starts the attract demo.
  The update check is skipped in split mode, which has to work offline.
- **Rumble.** Player 2 asks player 1 to rumble player 2's pad, with the same strength curve.
- **No mini opponent board** in a split battle: the other half is the opponent. CPU Battle keeps it.
- **No display** (started over SSH, say): the game exits with a message instead of falling back to
  SDL's offscreen driver and playing sound with no window. An explicit `SDL_VIDEODRIVER` is left alone.

## Battles over a local socket

`LocalLink` (`include/localLink.hpp`, `source/localLink.cpp`) replaces WebRTC while split. It is a
subclass of `LinkUniversal` over a second socketpair, so upstream's battle state machine, ready-up,
first-to-N, garbage, KO, results and Play Again run unchanged. The only change to upstream there is that
`LinkUniversal`'s public methods are `virtual`. Player 1 swaps a `LocalLink` + `MultiplayerLink` pair in
while split (`useLocalLink()`, `source/liba_pc.cpp`) and leaves the online pair untouched.

- Player 1 is player id 0 and deals the seed the room server would. Later rounds re-seed the upstream
  way (`lastSeed + 1` on both sides). Nothing opens a network socket.
- A side queues and sends INVITE. A side that is not in the queue screen is queued by
  `MatchmakingQueue::update()` and gets upstream's MATCH FOUND over a menu or a running game. In split
  screen MATCH FOUND starts on Accept, so one press accepts; online it keeps upstream's nothing-selected
  start, so a stray press in a game cannot accept.
- Declining is final for that invite: automatic rejoins send HELLO, not INVITE.
- Each side numbers its lobby sessions. Messages carry the sender's session and the one they are meant
  for, so leftovers from a lobby either side already left are dropped. Player 2 exiting reads as leaving.
- Lobby names are listed in player order on both sides (upstream iterated an `unordered_map`).
- `split screen battle: paired, seed N`, `player N left` and `split screen: on/off` go to stderr.

## Build

```
git submodule update --init --recursive
meson setup build-linux --buildtype=release -Dshared_sdl=true -DSoLoud:portmaster=true
meson compile -C build-linux
build-linux/Apotris
```

`shared_sdl` and `SoLoud:portmaster` link the system's shared SDL2, so the binary runs where it was built.
Keep it next to its `assets/` folder: assets are found relative to the executable.

For a release that runs on other distros, build the way upstream's release workflow does, in its Linux
release image (Ubuntu 20.04, SDL linked in, glibc 2.29 or newer needed), with Podman. `tools/build-appimage.sh`
then packages an AppImage:

```
podman run --rm -v "$PWD":/root/apotris -w /root/apotris docker.io/akouzoukos/apotris-linux:2026-5-28-2 \
    bash -c 'meson setup build-release -Db_lto=true -Db_lto_mode=thin --buildtype=release &&
             meson compile -C build-release && strip build-release/Apotris &&
             tools/build-appimage.sh build-release'
```

`.github/workflows/linux-release.yml` does the same on GitHub Actions for x86_64 and aarch64 (upstream's
`apotris-linux` and `apotris-arm64` images, the arm64 one on GitHub's ARM runner) and attaches an AppImage
and a tarball for each to the release. It runs when a release is published, or by hand from the Actions
tab (Linux release > Run workflow, with the tag).

## Tests

Everything runs on a private Xvfb with a throwaway `HOME` and no audio device. Real pads are hidden, and
two fake ones live inside the game process through `LD_PRELOAD` (`tools/split-fake-pads.c`), so no
system-wide virtual input device is created.

- `tools/split-test.sh OUT [SAVE] [WxH]` runs a scenario and writes screenshots to `OUT`.
  - Default (`--split`): both halves in one lobby, ready up, each pad and the keyboard (F8, held key)
    move only their own piece, player 2 tops out, then player 1's Main Menu closes player 2.
  - `SPLIT_TEST_SCENARIO=battle`: round 2 through Play Again on the same seed, still split.
  - `SPLIT_TEST_SCENARIO=toggle`: starts as one player; Multi Battle > Split Screen (player 2 joins the
    lobby), B out of it (player 2 saves and closes), then Split Screen again.
  - `SPLIT_TEST_SCENARIO=keys`: the same with the keyboard only, for a release build with static SDL
    (`SPLIT_TEST_BIN=build-release/Apotris`), which the fake pads cannot reach.
  - `SPLIT_TEST_QUIT=term|window|child` picks how it ends; with `toggle`, `child` checks player 2 dying.
  - `SPLIT_TEST_OFFLINE=1` refuses every IPv4/IPv6 socket in both processes and counts attempts.
  - `SPLIT_TEST_BOTS=N` with a debug build (`SPLIT_TEST_BIN=build-test/Apotris`) hands both sides to the
    bot (SELECT in the lobby) for N seconds, which is how garbage gets tested.
  - `SPLIT_TEST_AUDIO=disk`, `SPLIT_TEST_IDLE=N`, `SPLIT_TEST_NO_DISPLAY=1`, `SPLIT_TEST_HOLD=N` (for
    `tools/split-drive.sh`) and `SPLIT_TEST_GDB=1` help with debugging.
- `tools/split-test-single.sh OUT [SAVE]` checks single player: one process, any pad drives it.

The bundled shaders have no `#version`, so Mesa rejects them and the game falls back to the SDL path,
with or without this fork. To test the GL path, set `settings.shaders` (byte 696 of the save) to 2 and run
with `force_glsl_version=130`.

Tested on Xvfb only; play with two real controllers is not confirmed yet.

## Not done

Two boards in one process. Online play while split. Locking player 2's frames to player 1's vsync. A
vertical split. Platforms other than Linux (split screen needs fork/exec, memfd and Unix sockets).
